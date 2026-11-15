#include "Json.hpp"

namespace printbot::core {

using nlohmann::json;

const char *to_string(SliceSnapshot::State s)
{
    switch (s) {
    case SliceSnapshot::State::Idle: return "idle";
    case SliceSnapshot::State::Running: return "running";
    case SliceSnapshot::State::Done: return "done";
    case SliceSnapshot::State::Error: return "error";
    case SliceSnapshot::State::Cancelled: return "cancelled";
    }
    return "unknown";
}

json to_json(const Error &e) { return json{{"code", e.code}, {"message", e.message}}; }

json to_json(const PresetSnapshot &s)
{
    return json{{"revision", s.rev},
                {"printer", s.printer},
                {"process", s.process},
                {"filaments", s.filaments},
                {"bed_type", s.bed_type},
                {"printer_model", s.printer_model},
                {"nozzle_diameters", s.nozzle_diameters},
                {"extruder_count", s.extruder_count},
                {"bed", {{"min", s.bed_min}, {"max", s.bed_max}, {"max_print_height", s.max_print_height}}}};
}

json to_json(const ObjectInfo &o)
{
    std::array<double, 3> size{o.max[0] - o.min[0], o.max[1] - o.min[1], o.max[2] - o.min[2]};
    return json{{"id", o.id},           {"name", o.name},     {"source", o.source}, {"instances", o.instances},
                {"volumes", o.volumes}, {"facets", o.facets}, {"bbox_min", o.min},  {"bbox_max", o.max},
                {"size", size},         {"print_volume", o.print_volume}};
}

json to_json(const ProjectSnapshot &s)
{
    json objs = json::array();
    for (const auto &o : s.objects) objs.push_back(to_json(o));
    return json{{"revision", s.rev}, {"objects", objs}};
}

json to_json(const SliceSummary &s)
{
    json fil = json::array();
    for (const auto &f : s.filaments)
        fil.push_back(json{{"index", f.index}, {"preset", f.preset}, {"type", f.type}, {"used_mm", f.used_mm}, {"used_g", f.used_g}});
    json warn = json::array();
    for (const auto &w : s.warnings) warn.push_back(json{{"level", w.level}, {"code", w.code}, {"message", w.message}});
    return json{{"print_time_s", s.print_time_s},
                {"print_time", s.print_time},
                {"prepare_time_s", s.prepare_time_s},
                {"filament_used_m", s.filament_used_mm / 1000.0},
                {"filament_used_g", s.filament_used_g},
                {"filament_cost", s.filament_cost},
                {"filaments", fil},
                {"layer_count", s.layer_count},
                {"max_z_mm", s.max_z},
                {"objects", s.objects},
                {"warnings", warn},
                {"gcode_bytes", s.gcode_bytes},
                {"slice_wall_time_s", s.wall_time_s}};
}

json to_json(const SliceSnapshot &s)
{
    json j{{"state", to_string(s.state)}, {"job", s.job},           {"percent", s.percent},
           {"message", s.message},        {"outdated", s.outdated}, {"project_revision", s.project_rev},
           {"presets_revision", s.presets_rev}};
    if (s.summary) j["summary"] = to_json(*s.summary);
    if (s.error) j["error"] = to_json(*s.error);
    return j;
}

} // namespace printbot::core
