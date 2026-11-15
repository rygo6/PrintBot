// print_bot MCP tools. Each tool is a thin adapter from JSON arguments to a core service
// call; all state lives in the core. Phase 1: presets, model loading, slicing, G-code
// export. There are deliberately no printer connection / control / print tools yet.
#include <algorithm>

#include "McpServer.hpp"
#include "McpArgs.hpp"

#include "core/Core.hpp"
#include "core/Json.hpp"
#include "core/PresetService.hpp"
#include "core/ProjectService.hpp"
#include "core/SliceEvents.hpp"
#include "core/SliceService.hpp"
#include "core/Version.hpp"

namespace printbot::mcp {

using namespace printbot::core;
using namespace printbot::mcp::args;

namespace {

[[noreturn]] void fail(const Error &e) { throw ToolError{e.code, e.message}; }

json slice_brief(const SliceSnapshot &s)
{
    json j = to_json(s);
    return j;
}

json list_presets(ToolContext &ctx, const json &a, PresetService::Kind kind)
{
    PresetService::ListQuery q;
    q.kind            = kind;
    q.filter          = str_arg(a, "filter");
    q.compatible_only = kind == PresetService::Kind::Printer ? false : bool_arg(a, "compatible_only", true);
    q.visible_only    = bool_arg(a, "visible_only", false);
    q.limit           = size_t(std::clamp(num_arg(a, "limit", 300), 1.0, 5000.0));
    auto r            = ctx.core.presets().list(q);
    if (!r) fail(r.error());
    json arr = json::array();
    for (const auto &p : r.value().presets) {
        json j = {{"name", p.name}, {"system", p.system}, {"user", p.user}, {"visible", p.visible}, {"selected", p.selected}};
        if (kind == PresetService::Kind::Printer) {
            j["printer_model"] = p.printer_model;
            j["nozzle"]        = p.nozzle;
        } else {
            j["compatible"] = p.compatible;
        }
        if (kind == PresetService::Kind::Filament) {
            j["type"]   = p.filament_type;
            j["vendor"] = p.filament_vendor;
        }
        if (!p.alias.empty() && p.alias != p.name) j["alias"] = p.alias;
        arr.push_back(std::move(j));
    }
    auto snap = ctx.core.presets().snapshot();
    return json{{"returned", arr.size()},
                {"total_matching", r.value().total_matching},
                {"selected_printer", snap ? snap->printer : ""},
                {"presets", arr}};
}

// Start a slice and wait for it, forwarding progress to the client.
std::shared_ptr<const SliceSnapshot> slice_and_wait(ToolContext &ctx, double timeout_s)
{
    auto sub = ctx.core.events().subscribe({Topic::Slice}, [&ctx](const Event &ev) {
        if (ev.key == slice_events::kProgress) ctx.progress(ev.detail.value("percent", 0), ev.detail.value("text", ""));
    });
    auto job = ctx.core.slicer().start();
    if (!job) fail(job.error());
    auto snap = ctx.core.slicer().wait(job.value(), timeout_s);
    sub.reset();
    return snap;
}

json status(ToolContext &ctx)
{
    Core &core = ctx.core;
    json  j;
    j["server"] = {{"name", "print_bot"}, {"core_version", Core::version()}, {"slicer_version", Core::slicer_version()}};
    if (auto p = core.presets().snapshot()) j["presets"] = to_json(*p);
    if (auto p = core.project().snapshot()) j["project"] = to_json(*p);
    j["slice"]            = slice_brief(*core.slicer().snapshot());
    j["last_event_seq"]   = core.events().last_seq();
    j["startup_warnings"] = core.startup_warnings();
    j["mode"]             = "headless";
    j["printer_tools"]    = "not available headless: start the print_bot app for printers, plates, UI and print tools (print_bot-mcp then bridges to it)";
    return j;
}

} // namespace

std::vector<Tool> make_tools()
{
    std::vector<Tool> t;

    t.push_back({"get_status", "Get status",
                 "Current core state: versions, selected presets (printer/process/filaments/bed), loaded objects, slice state and summary.",
                 schema(), true, [](ToolContext &ctx, const json &) { return status(ctx); }});

    t.push_back({"get_version", "Get version", "Versions of the print_bot core API and the slicing engine.", schema(), true,
                 [](ToolContext &, const json &) {
                     return json{{"print_bot_core", Core::version()}, {"slicer_engine", Core::slicer_version()}};
                 }});

    const json filter_props = {{"filter", {{"type", "string"}, {"description", "Case-insensitive substring to match in the preset name."}}},
                               {"visible_only", {{"type", "boolean"}, {"description", "Only presets enabled in 'Printers and filaments'. Default false."}}},
                               {"limit", {{"type", "integer"}, {"description", "Max presets returned (default 300)."}}}};
    json compat_props                 = filter_props;
    compat_props["compatible_only"]   = {{"type", "boolean"}, {"description", "Only presets compatible with the selected printer. Default true."}};

    t.push_back({"list_printer_presets", "List printer presets",
                 "List printer presets (one per printer model and nozzle size, e.g. 'Bambu Lab P2S 0.4 nozzle').", schema(filter_props), true,
                 [](ToolContext &ctx, const json &a) { return list_presets(ctx, a, PresetService::Kind::Printer); }});
    t.push_back({"list_process_presets", "List process presets",
                 "List process (quality / layer height) presets, by default only those compatible with the selected printer.", schema(compat_props),
                 true, [](ToolContext &ctx, const json &a) { return list_presets(ctx, a, PresetService::Kind::Process); }});
    t.push_back({"list_filament_presets", "List filament presets",
                 "List filament presets, by default only those compatible with the selected printer.", schema(compat_props), true,
                 [](ToolContext &ctx, const json &a) { return list_presets(ctx, a, PresetService::Kind::Filament); }});

    t.push_back({"get_selected_presets", "Get selected presets", "The selected printer, process, filaments and bed type, plus the list of bed types.",
                 schema(), true, [](ToolContext &ctx, const json &) {
                     json j = to_json(*ctx.core.presets().snapshot());
                     j["available_bed_types"] = PresetService::bed_types();
                     return j;
                 }});

    t.push_back({"select_presets", "Select presets",
                 "Select presets. Any subset may be given; the printer is applied first. Changing the printer re-picks the process and filaments "
                 "only if they become incompatible (preferring the printer's defaults). 'filaments' sets the filament slots in order (slot 1 first).",
                 schema({{"printer", {{"type", "string"}, {"description", "Printer preset name, e.g. 'Bambu Lab P2S 0.4 nozzle'."}}},
                         {"process", {{"type", "string"}, {"description", "Process preset name, e.g. '0.20mm Standard @BBL P2S'."}}},
                         {"filaments", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Filament preset names, one per slot."}}},
                         {"bed_type", {{"type", "string"}, {"description", "Bed/plate type, e.g. 'Textured PEI Plate' (see get_selected_presets)."}}},
                         {"allow_incompatible", {{"type", "boolean"}, {"description", "Allow presets marked incompatible with the printer. Default false."}}}}),
                 false, [](ToolContext &ctx, const json &a) {
                     PresetService::Selection sel;
                     sel.printer  = opt_str(a, "printer");
                     sel.process  = opt_str(a, "process");
                     sel.bed_type = opt_str(a, "bed_type");
                     if (a.contains("filaments") && !a["filaments"].is_null()) {
                         std::vector<std::string> f;
                         if (a["filaments"].is_string()) f.push_back(a["filaments"].get<std::string>());
                         else if (a["filaments"].is_array()) {
                             for (const auto &x : a["filaments"]) {
                                 if (!x.is_string()) bad_arg("'filaments' must be an array of strings");
                                 f.push_back(x.get<std::string>());
                             }
                         } else bad_arg("'filaments' must be an array of strings");
                         sel.filaments = f;
                     }
                     sel.allow_incompatible = bool_arg(a, "allow_incompatible", false);
                     if (!sel.printer && !sel.process && !sel.filaments && !sel.bed_type) bad_arg("nothing to select");
                     auto r = ctx.core.presets().select(sel);
                     if (!r) fail(r.error());
                     return to_json(r.value());
                 }});

    t.push_back({"load_model", "Load model",
                 "Load a model file (STL, 3MF, OBJ, STEP, AMF, SVG) from a path on this machine. Replaces the current objects unless append is true. "
                 "Meshes are placed on the bed (centered / auto-arranged); 3MF objects keep their positions unless arrange is true. "
                 "Settings stored in a 3MF are not applied; select presets explicitly.",
                 schema({{"path", {{"type", "string"}, {"description", "Absolute path (or relative to the server's working directory)."}}},
                         {"append", {{"type", "boolean"}, {"description", "Add to the existing objects instead of replacing them. Default false."}}},
                         {"arrange", {{"type", "boolean"}, {"description", "Place objects on the bed. Default: true for meshes, false for 3MF."}}}},
                        {"path"}),
                 false, [](ToolContext &ctx, const json &a) {
                     ProjectService::LoadOptions o;
                     o.path    = str_arg(a, "path", true);
                     o.append  = bool_arg(a, "append", false);
                     o.arrange = opt_bool(a, "arrange");
                     auto r    = ctx.core.project().load(o);
                     if (!r) fail(r.error());
                     json added = json::array();
                     for (const auto &oi : r.value().added) added.push_back(to_json(oi));
                     return json{{"added", added}, {"warnings", r.value().warnings}, {"project", to_json(r.value().project)}};
                 }});

    t.push_back({"list_objects", "List objects", "Objects currently on the plate, with ids, sizes (mm) and whether they are inside the printable volume.",
                 schema(), true, [](ToolContext &ctx, const json &) { return to_json(*ctx.core.project().snapshot()); }});

    t.push_back({"remove_object", "Remove object", "Remove one object by id (see list_objects).",
                 schema({{"id", {{"type", "integer"}, {"description", "Object id."}}}}, {"id"}), false, [](ToolContext &ctx, const json &a) {
                     if (!a.contains("id") || !a["id"].is_number_integer()) bad_arg("'id' must be an integer");
                     auto r = ctx.core.project().remove_object(a["id"].get<uint64_t>());
                     if (!r) fail(r.error());
                     return to_json(r.value());
                 }});

    t.push_back({"clear_project", "Clear project", "Remove all objects from the plate.", schema(), false, [](ToolContext &ctx, const json &) {
                     auto r = ctx.core.project().clear();
                     if (!r) fail(r.error());
                     return to_json(r.value());
                 }});

    t.push_back({"arrange_objects", "Arrange objects", "Auto-arrange all objects on the bed.",
                 schema({{"spacing_mm", {{"type", "number"}, {"description", "Minimum gap between objects in mm (default 6)."}}}}), false,
                 [](ToolContext &ctx, const json &a) {
                     auto r = ctx.core.project().arrange(num_arg(a, "spacing_mm", 6.0));
                     if (!r) fail(r.error());
                     return json{{"warnings", r.value().warnings}, {"project", to_json(r.value().project)}};
                 }});

    t.push_back({"slice", "Slice",
                 "Slice the plate with the selected presets. By default waits for completion and returns the summary: estimated print time, "
                 "filament use (m, g, per slot), layer count and warnings. Sends progress notifications when the client provides a progressToken.",
                 schema({{"wait", {{"type", "boolean"}, {"description", "Wait for the slice to finish (default true)."}}},
                         {"timeout_s", {{"type", "number"}, {"description", "Max seconds to wait (default 900)."}}}}),
                 false, [](ToolContext &ctx, const json &a) {
                     std::shared_ptr<const SliceSnapshot> snap;
                     if (bool_arg(a, "wait", true)) {
                         snap = slice_and_wait(ctx, num_arg(a, "timeout_s", 900));
                     } else {
                         auto job = ctx.core.slicer().start();
                         if (!job) fail(job.error());
                         snap = ctx.core.slicer().snapshot();
                     }
                     if (snap->state == SliceSnapshot::State::Error && snap->error) fail(*snap->error);
                     if (snap->state == SliceSnapshot::State::Cancelled) throw ToolError{"slice.cancelled", "slicing was cancelled"};
                     return slice_brief(*snap);
                 }});

    t.push_back({"get_slice_status", "Get slice status", "State of the current/last slice job (idle, running + percent, done + summary, error, cancelled).",
                 schema(), true, [](ToolContext &ctx, const json &) { return slice_brief(*ctx.core.slicer().snapshot()); }});

    t.push_back({"cancel_slice", "Cancel slice", "Cancel the running slice job.", schema(), false, [](ToolContext &ctx, const json &) {
                     auto r = ctx.core.slicer().cancel();
                     if (!r) fail(r.error());
                     return json{{"cancelled", true}};
                 }});

    t.push_back({"export_gcode", "Export G-code",
                 "Write the sliced G-code to a file path. Slices first if there is no up-to-date slice (unless slice_if_needed is false).",
                 schema({{"path", {{"type", "string"}, {"description", "Destination .gcode path; the directory must exist."}}},
                         {"overwrite", {{"type", "boolean"}, {"description", "Replace an existing file (default true)."}}},
                         {"slice_if_needed", {{"type", "boolean"}, {"description", "Slice first when needed (default true)."}}},
                         {"timeout_s", {{"type", "number"}, {"description", "Max seconds to wait for slicing (default 900)."}}}},
                        {"path"}),
                 false, [](ToolContext &ctx, const json &a) {
                     const std::string path = str_arg(a, "path", true);
                     auto              snap = ctx.core.slicer().snapshot();
                     const bool fresh = snap->state == SliceSnapshot::State::Done && !snap->outdated;
                     if (!fresh && bool_arg(a, "slice_if_needed", true)) {
                         if (snap->state == SliceSnapshot::State::Running && !snap->outdated)
                             snap = ctx.core.slicer().wait(snap->job, num_arg(a, "timeout_s", 900));
                         else
                             snap = slice_and_wait(ctx, num_arg(a, "timeout_s", 900));
                         if (snap->state == SliceSnapshot::State::Error && snap->error) fail(*snap->error);
                     }
                     auto r = ctx.core.slicer().export_gcode(path, bool_arg(a, "overwrite", true));
                     if (!r) fail(r.error());
                     json j = {{"path", r.value().path}, {"bytes", r.value().bytes}, {"job", r.value().job}};
                     if (snap->summary) {
                         j["print_time"]      = snap->summary->print_time;
                         j["filament_used_g"] = snap->summary->filament_used_g;
                     }
                     return j;
                 }});

    t.push_back({"get_events", "Get events",
                 "Core state-change events after a sequence number (presets.changed, project.changed, slice.state, slice.progress, ...). "
                 "Poll with since = last_seq from the previous call.",
                 schema({{"since", {{"type", "integer"}, {"description", "Return events with seq > since (default 0)."}}},
                         {"limit", {{"type", "integer"}, {"description", "Max events (default 100)."}}}}),
                 true, [](ToolContext &ctx, const json &a) {
                     auto evs = ctx.core.events().history_since(uint64_t(std::max(0.0, num_arg(a, "since", 0))),
                                                                size_t(std::clamp(num_arg(a, "limit", 100), 1.0, 500.0)));
                     json arr = json::array();
                     for (const auto &e : evs) arr.push_back(json{{"seq", e.seq}, {"topic", topic_name(e.topic)}, {"key", e.key}, {"detail", e.detail}});
                     return json{{"events", arr}, {"last_seq", ctx.core.events().last_seq()}};
                 }});

    return t;
}

} // namespace printbot::mcp
