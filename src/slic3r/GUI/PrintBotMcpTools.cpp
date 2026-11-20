// print_bot: MCP tools served by the running app ("live mode"). Every tool does its GUI work
// on the wx main thread through the same code paths as the user's clicks (preset combos,
// Tab::select_preset, Plater actions, Selection + do_move, toolbar events), so the window
// reflects whatever the agent does. Tool names match the headless server where they overlap.
#include "PrintBotMcp.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <map>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/evp.h>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dcmemory.h>
#include <wx/dcscreen.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/weakref.h>
#include <wx/glcanvas.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GLCanvas3D.hpp"
#include "GLToolbar.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "PresetComboBoxes.hpp"
#include "PresetSelectionDialog.hpp"
#include "Selection.hpp"
#include "SelectMachine.hpp"
#include "Jobs/PrintJob.hpp"
#include "mcp/PrintAuthorization.hpp"
#include "SyncAmsInfoDialog.hpp"
#include "Tab.hpp"
#include "ParamsDialog.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevBed.h"
#include "DeviceCore/DevChamber.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevHMS.h"
#include "Widgets/Button.hpp"

#include "core/Core.hpp"
#include "core/SliceEvents.hpp"
#include "core/Version.hpp"
#include "libslic3r_version.h"
#include "mcp/McpArgs.hpp"
#include "mcp/McpServer.hpp"
#include "mcp/Subprocess.hpp"

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI { namespace pbmcp {

using nlohmann::json;
using printbot::mcp::Tool;
using printbot::mcp::ToolContext;
using printbot::mcp::ToolError;
using namespace printbot::mcp::args;
namespace core = printbot::core;

namespace {

// Enforce the option's min/max like the settings panel does. Without this an agent could set e.g. wall_loops = 99999
// (max 1000), which overflows int math in the slicer.
void check_option_range(const ConfigBase &cfg, const std::string &key, const char *code)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    const ConfigOption    *opt = cfg.option(key);
    if (def == nullptr || opt == nullptr) return;
    if (def->min == ConfigOptionDef::min_default && def->max == ConfigOptionDef::max_default) return;
    auto check = [&](double v) {
        if (std::isnan(v) || v < def->min || v > def->max) {
            std::string range = "[" + (def->min == ConfigOptionDef::min_default ? std::string("-inf") : float_to_string_decimal_point(def->min)) + ", " +
                                (def->max == ConfigOptionDef::max_default ? std::string("inf") : float_to_string_decimal_point(def->max)) + "]";
            throw ToolError{code, key + ": value " + float_to_string_decimal_point(v) + " is outside the allowed range " + range};
        }
    };
    switch (opt->type()) {
    case coInt: check(double(static_cast<const ConfigOptionInt *>(opt)->value)); break;
    case coFloat: check(static_cast<const ConfigOptionFloat *>(opt)->value); break;
    case coFloatOrPercent:
        if (!static_cast<const ConfigOptionFloatOrPercent *>(opt)->percent) check(static_cast<const ConfigOptionFloatOrPercent *>(opt)->value);
        break;
    case coInts:
        for (int v : static_cast<const ConfigOptionInts *>(opt)->values) check(double(v));
        break;
    case coFloats:
        for (double v : static_cast<const ConfigOptionFloats *>(opt)->values) check(v);
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// basics

std::string u8(const wxString &s) { return std::string(s.ToUTF8().data()); }
wxString    wx(const std::string &s) { return wxString::FromUTF8(s.c_str()); }

Plater &plater()
{
    Plater *p = wxGetApp().plater();
    if (!p || !wxGetApp().mainframe) throw ToolError{"app.not_ready", "the print_bot window is not ready yet"};
    return *p;
}

PresetBundle &bundle() { return *wxGetApp().preset_bundle; }

json on_main(std::function<json()> fn, double timeout_s = 120.0) { return run_on_main(std::move(fn), timeout_s); }

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

double elapsed_s(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

void require_policy(const char *what)
{
    if (!printer_control_allowed())
        throw ToolError{"policy.denied",
                        std::string(what) +
                            " is disabled. The user must enable printer control for agents first (start print_bot with "
                            "PRINTBOT_MCP_ALLOW_PRINTER_CONTROL=1, or set mcp_allow_printer_control = true in the app config)."};
}

// Plate numbers in the API are 1-based, like the "Plate 1" labels in the app.
int plate_index_arg(const json &a, const char *key, bool required = false)
{
    long long n = int_arg(a, key, required, 0);
    PartPlateList &pl = plater().get_partplate_list();
    if (n == 0) {
        // A required plate must be a real 1-based number; an omitted optional one means "current plate".
        if (required) bad_arg(std::string("'") + key + "' must be a plate number from 1 to " + std::to_string(pl.get_plate_count()));
        return pl.get_curr_plate_index();
    }
    if (n < 1 || n > pl.get_plate_count()) bad_arg("plate " + std::to_string(n) + " does not exist (there are " + std::to_string(pl.get_plate_count()) + ")");
    return int(n - 1);
}

// ---------------------------------------------------------------------------------------------
// objects

int object_index(uint64_t id)
{
    const Model &m = plater().model();
    for (size_t i = 0; i < m.objects.size(); ++i)
        if (m.objects[i]->id().id == id) return int(i);
    throw ToolError{"object.not_found", "no object with id " + std::to_string(id) + " (see list_objects)"};
}

json vec3(const Vec3d &v) { return json::array({v.x(), v.y(), v.z()}); }

json object_json(size_t idx)
{
    Plater            &p  = plater();
    const ModelObject *o  = p.model().objects[idx];
    json               j;
    j["id"]        = o->id().id;
    j["index"]     = idx;
    j["name"]      = o->name;
    j["instances"] = o->instances.size();
    j["volumes"]   = o->volumes.size();
    if (!o->instances.empty()) {
        const ModelInstance *inst = o->instances.front();
        BoundingBoxf3        bb   = o->instance_bounding_box(0);
        j["size_mm"]              = vec3(bb.size());
        j["center"]               = vec3(bb.center());
        j["min"]                  = vec3(bb.min);
        j["max"]                  = vec3(bb.max);
        j["position"]             = vec3(inst->get_offset());
        Vec3d rot                 = inst->get_rotation() * (180.0 / M_PI);
        j["rotation_deg"]         = vec3(rot);
        j["scale"]                = vec3(inst->get_scaling_factor());
        j["printable"]            = inst->printable;
        const int plate           = p.get_partplate_list().find_instance(int(idx), 0);
        j["plate"]                = plate >= 0 ? json(plate + 1) : json(nullptr);
        j["outside_print_volume"] = inst->print_volume_state != ModelInstancePVS_Inside;
    }
    j["filament"] = o->config.has("extruder") ? o->config.opt_int("extruder") : 0;
    json settings = json::object();
    for (const std::string &k : o->config.keys())
        if (k != "extruder") settings[k] = o->config.opt_serialize(k);
    j["settings"] = settings;
    return j;
}

json objects_json()
{
    json arr = json::array();
    for (size_t i = 0; i < plater().model().objects.size(); ++i) arr.push_back(object_json(i));
    return arr;
}

// Make `indices` the current selection in the 3D view (and the object list), like clicking them.
void select_objects_ui(const std::vector<int> &indices)
{
    Plater     &p      = plater();
    GLCanvas3D *canvas = p.canvas3D();
    Selection  &sel    = canvas->get_selection();
    sel.clear();
    for (int idx : indices) sel.add_object(unsigned(idx), false);
    wxGetApp().obj_list()->update_selections();
    canvas->set_as_dirty();
    canvas->request_extra_frame();
}

// ---------------------------------------------------------------------------------------------
// plates and slicing

std::string bed_type_name(BedType bt)
{
    if (bt == btDefault) return "default";
    const auto &names = print_config_def.get("curr_bed_type")->enum_values;
    int         i     = int(bt) - 1;
    return i >= 0 && i < int(names.size()) ? names[i] : std::to_string(int(bt));
}

json slice_summary(PartPlate *plate)
{
    json j;
    j["sliced"]             = plate->is_slice_result_valid();
    j["ready_for_print"]    = plate->is_slice_result_ready_for_print();
    GCodeProcessorResult *r = plate->get_slice_result();
    if (!plate->is_slice_result_valid() || !r) return j;
    using Mode        = PrintEstimatedStatistics::ETimeMode;
    const auto &mode  = r->print_statistics.modes[size_t(Mode::Normal)];
    j["print_time_s"] = mode.time;
    j["print_time"]   = get_time_dhms(mode.time);
    j["layer_count"]  = mode.layers_times.size();
    const DynamicPrintConfig cfg   = bundle().full_config();
    const auto              *diam  = cfg.option<ConfigOptionFloats>("filament_diameter");
    const auto              *dens  = cfg.option<ConfigOptionFloats>("filament_density");
    const auto              *type  = cfg.option<ConfigOptionStrings>("filament_type");
    double                   tot_m = 0, tot_g = 0;
    json                     fil   = json::array();
    for (const auto &[idx, volume] : r->print_statistics.total_volumes_per_extruder) {
        const double d  = diam && idx < diam->values.size() ? diam->values[idx] : 1.75;
        const double mm = volume / (M_PI * d * d / 4.0);
        const double g  = volume * (dens && idx < dens->values.size() ? dens->values[idx] : 1.24) / 1000.0;
        tot_m += mm / 1000.0;
        tot_g += g;
        fil.push_back({{"slot", idx + 1},
                       {"preset", idx < bundle().filament_presets.size() ? bundle().filament_presets[idx] : ""},
                       {"type", type && idx < type->values.size() ? type->values[idx] : ""},
                       {"used_m", mm / 1000.0},
                       {"used_g", g}});
    }
    j["filament_used_m"] = tot_m;
    j["filament_used_g"] = tot_g;
    j["filaments"]       = fil;
    json warnings        = json::array();
    for (const auto &w : r->warnings)
        warnings.push_back({{"level", w.level >= 2 ? "error" : w.level == 1 ? "warning" : "info"}, {"code", w.error_code}, {"msg", w.msg}});
    j["warnings"] = warnings;
    return j;
}

json plate_json(int i, bool with_slice = true)
{
    Plater        &p     = plater();
    PartPlateList &pl    = p.get_partplate_list();
    if (i < 0) i = pl.get_curr_plate_index();
    PartPlate     *plate = pl.get_plate(i);
    if (!plate) throw ToolError{"plate.not_found", "plate " + std::to_string(i + 1) + " does not exist"};
    json           j;
    j["plate"]          = i + 1;
    j["name"]           = plate->get_plate_name();
    j["current"]        = pl.get_curr_plate_index() == i;
    j["bed_type"]       = bed_type_name(plate->get_bed_type());
    j["print_sequence"] = plate->get_print_seq() == PrintSequence::ByDefault ? "default" :
                          plate->get_print_seq() == PrintSequence::ByObject  ? "by object" :
                                                                               "by layer";
    json objs = json::array();
    for (const auto &oi : plate->get_obj_and_inst_set())
        if (oi.second == 0 && oi.first >= 0 && oi.first < int(p.model().objects.size())) objs.push_back(p.model().objects[oi.first]->id().id);
    j["object_ids"] = objs;
    j["filaments"]  = plate->get_extruders();
    if (with_slice) j["slice"] = slice_summary(plate);
    return j;
}

json plates_json()
{
    json arr = json::array();
    for (int i = 0; i < plater().get_partplate_list().get_plate_count(); ++i) arr.push_back(plate_json(i));
    return arr;
}

// ---------------------------------------------------------------------------------------------
// presets

PresetCollection &collection(Preset::Type t)
{
    switch (t) {
    case Preset::TYPE_PRINTER: return bundle().printers;
    case Preset::TYPE_PRINT: return bundle().prints;
    default: return bundle().filaments;
    }
}

Preset::Type type_arg(const json &a, const char *key = "type")
{
    const std::string t = boost::algorithm::to_lower_copy(str_arg(a, key, true));
    if (t == "printer" || t == "machine") return Preset::TYPE_PRINTER;
    if (t == "process" || t == "print") return Preset::TYPE_PRINT;
    if (t == "filament") return Preset::TYPE_FILAMENT;
    bad_arg("'type' must be printer, process or filament");
}

const char *type_name(Preset::Type t) { return t == Preset::TYPE_PRINTER ? "printer" : t == Preset::TYPE_PRINT ? "process" : "filament"; }

std::string resolve_preset(Preset::Type t, const std::string &name)
{
    PresetCollection &c = collection(t);
    if (c.find_preset(name, false)) return name;
    const std::string by_alias = bundle().get_preset_name_by_alias(t, name);
    if (c.find_preset(by_alias, false)) return by_alias;
    std::vector<std::string> hits;
    for (const Preset &p : c) {
        if (boost::iequals(p.name, name)) return p.name;
        if (boost::icontains(p.name, name)) hits.push_back(p.name);
    }
    if (hits.size() == 1) return hits.front();
    std::string msg = std::string("no ") + type_name(t) + " preset named '" + name + "'";
    if (!hits.empty()) {
        msg += "; did you mean: ";
        for (size_t i = 0; i < hits.size() && i < 8; ++i) msg += (i ? ", '" : "'") + hits[i] + "'";
    }
    throw ToolError{"preset.not_found", msg};
}

json selected_presets_json()
{
    PresetBundle &b = bundle();
    json          j;
    j["printer"]         = b.printers.get_selected_preset_name();
    j["process"]         = b.prints.get_selected_preset_name();
    j["filaments"]       = b.filament_presets;
    j["printer_dirty"]   = b.printers.current_is_dirty();
    j["process_dirty"]   = b.prints.current_is_dirty();
    j["filament_dirty"]  = b.filaments.current_is_dirty();
    j["edited_filament"] = b.filaments.get_edited_preset().name;
    if (b.project_config.has("curr_bed_type")) j["bed_type"] = bed_type_name(b.project_config.opt_enum<BedType>("curr_bed_type"));
    j["printer_model"] = b.printers.get_edited_preset().config.opt_string("printer_model");
    return j;
}

json list_presets(const json &a, Preset::Type t)
{
    const std::string filter     = str_arg(a, "filter");
    const bool        compatible = t == Preset::TYPE_PRINTER ? false : bool_arg(a, "compatible_only", true);
    const bool        visible    = bool_arg(a, "visible_only", false);
    const size_t      limit      = size_t(std::clamp(num_arg(a, "limit", 300), 1.0, 5000.0));
    return on_main([=]() {
        PresetCollection &c        = collection(t);
        const std::string selected = c.get_selected_preset_name();
        json              arr      = json::array();
        size_t            matching = 0;
        for (const Preset &p : c) {
            if (p.is_default && !p.is_visible) continue;
            if (compatible && !p.is_compatible) continue;
            if (visible && !p.is_visible) continue;
            if (!filter.empty() && !boost::icontains(p.name, filter) && !boost::icontains(p.alias, filter)) continue;
            ++matching;
            if (arr.size() >= limit) continue;
            json j = {{"name", p.name}, {"system", p.is_system}, {"user", p.is_user()}, {"visible", p.is_visible}, {"selected", p.name == selected}};
            if (t == Preset::TYPE_PRINTER) {
                j["printer_model"] = p.config.opt_string("printer_model");
                j["nozzle"]        = p.config.opt_string("printer_variant");
            } else {
                j["compatible"] = p.is_compatible;
            }
            if (t == Preset::TYPE_FILAMENT) {
                if (auto *o = p.config.option<ConfigOptionStrings>("filament_type"); o && !o->values.empty()) j["type"] = o->values[0];
                if (auto *o = p.config.option<ConfigOptionStrings>("filament_vendor"); o && !o->values.empty()) j["vendor"] = o->values[0];
            }
            if (!p.alias.empty() && p.alias != p.name) j["alias"] = p.alias;
            arr.push_back(std::move(j));
        }
        return json{{"returned", arr.size()}, {"total_matching", matching}, {"selected", selected}, {"presets", arr}};
    });
}

// Select an item of a preset combo box like a user click (fires the same wxEVT_COMBOBOX).
bool select_in_combo(PlaterPresetComboBox *combo, Preset::Type t, const std::string &name)
{
    if (!combo) return false;
    for (unsigned i = 0; i < combo->GetCount(); ++i) {
        const std::string item = Preset::remove_suffix_modified(u8(combo->GetString(i)));
        if (item != name && bundle().get_preset_name_by_alias(t, item) != name) continue;
        combo->SetSelection(int(i));
        wxCommandEvent evt(wxEVT_COMBOBOX, combo->GetId());
        evt.SetEventObject(combo);
        evt.SetInt(int(i));
        combo->GetEventHandler()->ProcessEvent(evt);
        return true;
    }
    return false;
}

void select_printer_ui(const std::string &name)
{
    Plater &p = plater();
    if (bundle().printers.get_selected_preset_name() == name) return;
    if (Preset *pr = bundle().printers.find_preset(name, false)) pr->is_visible = true;
    if (select_in_combo(p.sidebar().printer_combox(), Preset::TYPE_PRINTER, name)) return;
    // Not listed in the combo: same steps as Plater's preset selection.
    bundle().physical_printers.unselect_printer();
    p.update_objects_position_when_select_preset([&]() {
        wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(name);
        p.on_config_change(bundle().full_config());
    });
}

void set_filament_slot_ui(size_t idx, const std::string &name)
{
    Plater &p      = plater();
    auto   &combos = p.sidebar().combos_filament();
    if (idx < bundle().filament_presets.size() && bundle().filament_presets[idx] == name) return;
    if (Preset *pr = bundle().filaments.find_preset(name, false)) pr->is_visible = true;
    if (idx < combos.size()) {
        combos[idx]->update();
        if (select_in_combo(combos[idx], Preset::TYPE_FILAMENT, name)) return;
    }
    // Fallback (preset hidden from the combo): same steps as Plater::priv::on_select_preset.
    const std::string old = idx < bundle().filament_presets.size() ? bundle().filament_presets[idx] : "";
    bundle().set_filament_preset(idx, name);
    if (!p.on_filament_change(idx)) bundle().set_filament_preset(idx, old);
    p.update_project_dirty_from_presets();
    bundle().export_selections(*wxGetApp().app_config);
    if (idx < combos.size()) combos[idx]->update();
    p.on_config_change(bundle().full_config());
}

Tab *tab_for(Preset::Type t)
{
    Tab *tab = wxGetApp().get_tab(t);
    if (!tab) throw ToolError{"app.not_ready", "settings tab not available"};
    return tab;
}

// ---------------------------------------------------------------------------------------------
// printers

DeviceManager *devices()
{
    DeviceManager *d = wxGetApp().getDeviceManager();
    if (!d) throw ToolError{"printer.unavailable", "printer support is not initialized (network plugin not loaded)"};
    return d;
}

json printer_brief(MachineObject *o, MachineObject *selected)
{
    json j;
    j["id"]              = o->get_dev_id();
    j["name"]            = o->get_dev_name();
    j["ip"]              = o->get_dev_ip();
    j["model"]           = o->get_show_printer_type();
    j["online"]          = o->is_online();
    j["lan_mode"]        = o->is_lan_mode_printer();
    j["has_access_code"] = o->has_access_right();
    j["connected"]       = o->is_connected();
    j["connecting"]      = o->is_connecting();
    j["selected"]        = o == selected;
    j["busy"]            = o->is_in_printing();
    return j;
}

json printer_status(MachineObject *o)
{
    json j             = printer_brief(o, devices()->get_selected_machine());
    j["print_status"]  = o->print_status;
    j["task_name"]     = o->subtask_name;
    j["progress_pct"]  = o->mc_print_percent;
    j["remaining_min"] = o->mc_left_time / 60;
    j["layer"]         = o->curr_layer;
    j["total_layers"]  = o->total_layers;
    j["print_error"]   = o->print_error;
    j["can_pause"]     = o->can_pause();
    j["can_resume"]    = o->can_resume();
    j["can_stop"]      = o->can_abort();
    j["wifi_signal"]   = o->wifi_signal;
    json temps         = json::object();
    if (DevBed *bed = o->GetBed()) temps["bed"] = {{"current", bed->GetBedTemp()}, {"target", bed->GetBedTempTarget()}};
    if (auto ch = o->GetChamber()) temps["chamber"] = {{"current", ch->GetChamberTemp()}, {"target", ch->GetChamberTempTarget()}};
    if (DevExtderSystem *ex = o->GetExtderSystem()) {
        json n = json::array();
        for (const DevExtder &e : ex->GetExtruders()) n.push_back({{"current", e.GetCurrentTemp()}, {"target", e.GetTargetTemp()}});
        temps["nozzles"] = n;
    }
    j["temperatures"] = temps;
    json ams          = json::array();
    const auto filament_slots = wxGetApp().sidebar().build_filament_ams_list(o);
    if (auto fsys = o->GetFilaSystem()) {
        for (auto &[ams_id, unit] : fsys->GetAmsList()) {
            if (!unit) continue;
            json trays = json::array();
            for (auto &[tray_id, tray] : unit->GetTrays()) {
                if (!tray) continue;
                std::string label;
                for (const auto &entry : filament_slots) {
                    const auto &slot = entry.second;
                    if (slot.opt_string("ams_id", 0u) == ams_id && slot.opt_string("slot_id", 0u) == tray_id) {
                        label = slot.opt_string("tray_name", 0u);
                        break;
                    }
                }
                trays.push_back({{"slot", tray_id}, {"label", label},
                                 {"loaded", tray->is_exists},
                                 {"type", tray->get_display_filament_type()},
                                 {"brand", tray->sub_brands},
                                 {"color", tray->color},
                                 {"remain_pct", tray->remain},
                                 {"filament_id", tray->setting_id}});
            }
            ams.push_back({{"id", ams_id}, {"display_name", u8(unit->GetDisplayName())}, {"humidity_pct", unit->GetHumidityPercent()}, {"temperature", unit->GetCurrentTemperature()}, {"trays", trays}});
        }
    }
    j["ams"] = ams;
    json hms = json::array();
    if (DevHMS *h = o->GetHMS())
        for (const auto &item : h->GetHMSItems()) hms.push_back({{"code", item.get_long_error_code()}, {"level", int(item.get_level())}});
    j["hms"] = hms;
    return j;
}

MachineObject *printer_arg(const json &a, bool required_connected = false)
{
    DeviceManager    *dev = devices();
    const std::string id  = str_arg(a, "printer");
    MachineObject    *o   = nullptr;
    if (id.empty()) {
        o = dev->get_selected_machine();
        if (!o) throw ToolError{"printer.none_selected", "no printer is selected; pass 'printer' (see list_printers) or connect_printer first"};
    } else {
        for (auto &[dev_id, m] : dev->get_local_machinelist())
            if (m && (dev_id == id || m->get_dev_name() == id || m->get_dev_ip() == id)) o = m;
        if (!o) o = dev->get_my_machine(id);
        if (!o) throw ToolError{"printer.not_found", "no discovered printer '" + id + "' (see list_printers)"};
    }
    if (required_connected && !o->is_connected()) throw ToolError{"printer.not_connected", "printer '" + o->get_dev_name() + "' is not connected"};
    return o;
}

// ---------------------------------------------------------------------------------------------
// UI: tabs, dialogs, widgets

const char *tab_name(int sel)
{
    switch (sel) {
    case MainFrame::tpHome: return "home";
    case MainFrame::tp3DEditor: return "prepare";
    case MainFrame::tpPreview: return "preview";
    case MainFrame::tpMonitor: return "device";
    default: return "unknown";
    }
}

std::string window_kind(wxWindow *w) { return u8(w->GetClassInfo() ? wxString(w->GetClassInfo()->GetClassName()) : wxString("wxWindow")); }

std::string dialog_class(wxWindow *w)
{
    // RTTI name is more specific than wxClassInfo for our DPIDialog subclasses.
    std::string n = typeid(*w).name();
    for (const char *k : {"SelectMachineDialog", "SendToPrinterDialog", "PreferencesDialog", "PresetSelectionDialog", "ParamsDialog",
                          "MessageDialog", "ConnectPrinterDialog", "UnsavedChangesDialog", "SavePresetDialog", "PlateSettingsDialog",
                          "SyncNozzleAndAmsDialog", "SyncAmsInfoDialog", "FinishSyncAmsDialog"})
        if (n.find(k) != std::string::npos) return k;
    return window_kind(w);
}

std::vector<wxTopLevelWindow *> shown_dialogs()
{
    std::vector<wxTopLevelWindow *> out;
    for (wxWindow *w : wxTopLevelWindows) {
        auto *tlw = dynamic_cast<wxTopLevelWindow *>(w);
        if (!tlw || !tlw->IsShown() || tlw == wxGetApp().mainframe) continue;
        if (!dynamic_cast<wxDialog *>(tlw) && tlw->GetTitle().empty() &&
            !dynamic_cast<SyncNozzleAndAmsDialog *>(tlw) && !dynamic_cast<FinishSyncAmsDialog *>(tlw)) continue; // popups / tooltips
        out.push_back(tlw);
    }
    return out;
}

json dialog_json(wxTopLevelWindow *w, size_t i)
{
    auto *d = dynamic_cast<wxDialog *>(w);
    return json{{"index", i}, {"title", u8(w->GetTitle())}, {"class", dialog_class(w)}, {"modal", d && d->IsModal()}, {"topmost", i + 1 == shown_dialogs().size()}};
}

wxTopLevelWindow *find_dialog(const json &a)
{
    auto        dlgs = shown_dialogs();
    std::string key  = a.contains("dialog") && a["dialog"].is_number_integer() ? std::to_string(a["dialog"].get<long long>()) : str_arg(a, "dialog");
    if (dlgs.empty()) throw ToolError{"ui.no_dialog", "no dialog is open"};
    if (key.empty()) return dlgs.back();
    for (size_t i = 0; i < dlgs.size(); ++i)
        if (std::to_string(i) == key || boost::iequals(u8(dlgs[i]->GetTitle()), key)) return dlgs[i];
    for (auto *d : dlgs)
        if (boost::icontains(u8(d->GetTitle()), key) || boost::icontains(dialog_class(d), key)) return d;
    throw ToolError{"ui.dialog_not_found", "no open dialog matches '" + key + "' (see ui_list_dialogs)"};
}

// Dialogs that start prints or send files to a printer: acting in them is policy-gated.
bool is_print_dialog(wxWindow *w)
{
    const std::string k = dialog_class(w);
    return k.find("SelectMachine") != std::string::npos || k.find("SendToPrinter") != std::string::npos;
}
bool is_print_window(wxWindow *w);

std::string widget_label(wxWindow *w)
{
    std::string l = u8(w->GetLabel());
    if (l.empty()) l = u8(w->GetName());
    return l;
}

bool is_listed_widget(wxWindow *c)
{
    return dynamic_cast<wxTextCtrl *>(c) || dynamic_cast<wxCheckBox *>(c) || dynamic_cast<wxRadioButton *>(c) || dynamic_cast<wxChoice *>(c) ||
           dynamic_cast<wxComboBox *>(c) || dynamic_cast<wxButton *>(c) || dynamic_cast<Button *>(c) ||
           (dynamic_cast<wxStaticText *>(c) && !c->GetLabel().empty());
}

void collect_widgets(wxWindow *w, json &out, int depth)
{
    for (wxWindow *c : w->GetChildren()) {
        if (!c->IsShown()) continue;
        if (is_listed_widget(c)) {
            json j;
            if (auto *t = dynamic_cast<wxTextCtrl *>(c)) {
                j["kind"]  = "text";
                j["value"] = u8(t->GetValue());
            } else if (auto *cb = dynamic_cast<wxCheckBox *>(c)) {
                j["kind"]  = "checkbox";
                j["value"] = cb->GetValue();
                j["label"] = u8(cb->GetLabel());
            } else if (auto *rb = dynamic_cast<wxRadioButton *>(c)) {
                j["kind"]  = "radio";
                j["value"] = rb->GetValue();
                j["label"] = u8(rb->GetLabel());
            } else if (auto *co = dynamic_cast<wxComboBox *>(c)) {
                j["kind"]  = "combo";
                j["value"] = u8(co->GetValue());
                json opts  = json::array();
                for (unsigned i = 0; i < co->GetCount() && i < 60; ++i) opts.push_back(u8(co->GetString(i)));
                j["options"] = opts;
            } else if (auto *ch = dynamic_cast<wxChoice *>(c)) {
                j["kind"]  = "choice";
                j["value"] = u8(ch->GetStringSelection());
                json opts  = json::array();
                for (unsigned i = 0; i < ch->GetCount() && i < 60; ++i) opts.push_back(u8(ch->GetString(i)));
                j["options"] = opts;
            } else if (dynamic_cast<wxButton *>(c) || dynamic_cast<Button *>(c)) {
                j["kind"]  = "button";
                j["label"] = widget_label(c);
            } else {
                j["kind"]  = "label";
                j["label"] = u8(c->GetLabel());
            }
            j["widget"]  = out.size();
            j["enabled"] = c->IsEnabled();
            out.push_back(std::move(j));
        }
        if (depth < 40) collect_widgets(c, out, depth + 1);
    }
}

// The same DFS order as collect_widgets, returning the n-th listed widget.
wxWindow *nth_widget(wxWindow *root, size_t n)
{
    size_t                                     count = 0;
    std::function<wxWindow *(wxWindow *, int)> rec   = [&](wxWindow *w, int depth) -> wxWindow * {
        for (wxWindow *c : w->GetChildren()) {
            if (!c->IsShown()) continue;
            if (is_listed_widget(c) && count++ == n) return c;
            if (depth < 40)
                if (wxWindow *r = rec(c, depth + 1)) return r;
        }
        return nullptr;
    };
    return rec(root, 0);
}

wxWindow *find_button(wxWindow *root, const std::string &label)
{
    wxWindow                             *partial = nullptr;
    std::function<wxWindow *(wxWindow *)> rec     = [&](wxWindow *w) -> wxWindow * {
        for (wxWindow *c : w->GetChildren()) {
            if (!c->IsShown()) continue;
            if (dynamic_cast<wxButton *>(c) || dynamic_cast<Button *>(c)) {
                std::string l = widget_label(c);
                boost::algorithm::erase_all(l, "&");
                if (boost::iequals(l, label)) return c;
                if (!partial && boost::icontains(l, label)) partial = c;
            }
            if (wxWindow *r = rec(c)) return r;
        }
        return nullptr;
    };
    wxWindow *exact = rec(root);
    return exact ? exact : partial;
}

void click_window(wxWindow *c)
{
    // Generic MCP clicks never reach a Print / Send dialog: sending goes only through gated_send().
    if (is_print_window(c)) throw ToolError{"print.human_only", "buttons in the Print / Send dialogs are for the user only (use print_plate)"};
    wxCommandEvent evt(wxEVT_BUTTON, c->GetId());
    evt.SetEventObject(c);
    c->GetEventHandler()->ProcessEvent(evt);
}

bool write_png(const std::string &path, int w, int h, const std::vector<unsigned char> &rgba, std::string &err)
{
    wxImage        img(w, h);
    unsigned char *rgb = img.GetData();
    for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) wxImage::AddHandler(new wxPNGHandler());
    if (!img.SaveFile(wx(path), wxBITMAP_TYPE_PNG)) {
        err = "could not write " + path;
        return false;
    }
    return true;
}

std::string check_out_path(const json &a, const char *key, const char *ext)
{
    std::string path = str_arg(a, key, true);
    fs::path    p(path);
    if (!p.is_absolute()) bad_arg(std::string("'") + key + "' must be an absolute path");
    if (!p.parent_path().empty() && !fs::exists(p.parent_path())) bad_arg("directory does not exist: " + p.parent_path().string());
    if (ext && !boost::iends_with(path, ext)) path += ext;
    return path;
}

// Wait (off the main thread) until pred(), evaluated on the main thread, returns non-null.
json wait_until(std::function<json()> pred, double timeout_s, int poll_ms = 250)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        json r = on_main(pred, 30);
        if (!r.is_null()) return r;
        if (elapsed_s(t0) > timeout_s) return json();
        sleep_ms(poll_ms);
    }
}

// ---------------------------------------------------------------------------------------------
// slicing

struct SliceWatch
{
    std::mutex         m;
    int                completed = 0;
    int                status    = -1; // slice_events::ProcessCompleted::Status
    std::string        error;
    core::Subscription sub;
};

std::shared_ptr<SliceWatch> watch_slicing(ToolContext &ctx)
{
    auto                      w  = std::make_shared<SliceWatch>();
    std::weak_ptr<SliceWatch> ww = w;
    w->sub = ctx.core.events().subscribe({core::Topic::Slice}, [ww](const core::Event &ev) {
        auto w = ww.lock();
        if (!w || ev.key != core::slice_events::kProcessCompleted) return;
        std::lock_guard<std::mutex> lk(w->m);
        ++w->completed;
        if (auto *p = ev.payload_as<core::slice_events::ProcessCompleted>()) {
            w->status = int(p->status);
            if (p->exception) {
                try {
                    std::rethrow_exception(p->exception);
                } catch (const std::exception &e) {
                    w->error = e.what();
                } catch (...) {
                    w->error = "slicing failed";
                }
            }
        }
    });
    return w;
}

json slice(ToolContext &ctx, const json &a)
{
    const bool   all     = bool_arg(a, "all_plates", false);
    const double timeout = num_arg(a, "timeout_s", 900);
    const bool   wait    = bool_arg(a, "wait", true);
    const int    plate   = on_main([a]() { return json(plate_index_arg(a, "plate")); }).get<int>();
    auto         watch   = watch_slicing(ctx);
    on_main([=]() {
        Plater &p = plater();
        if (p.model().objects.empty()) throw ToolError{"slice.empty", "nothing to slice: there are no objects"};
        if (plate >= 0 && plate != p.get_partplate_list().get_curr_plate_index()) p.select_plate(plate);
        // Same steps as the "Slice plate" / "Slice all" button.
        if (p.is_background_process_update_scheduled()) p.update(false, true);
        p.reset_check_status();
        p.set_slice_from_slice_btn(true);
        p.exit_gizmo();
        p.update(true, true);
        if (!all) {
            bool fits = false, invalid = false;
            p.validate_current_plate(fits, invalid);
            if (!fits || invalid) {
                // Name the validation error when there is one (the app shows it as a notification).
                std::string why;
                if (invalid) {
                    StringObjectException err = p.get_partplate_list().get_current_fff_print().validate();
                    if (!err.string.empty()) {
                        p.post_process_string_object_exception(err);
                        why = ": " + err.string + (err.opt_key.empty() ? std::string() : " (setting '" + err.opt_key + "')");
                    }
                }
                if (!why.empty()) throw ToolError{"slice.invalid_plate", "the plate cannot be sliced" + why};
                throw ToolError{"slice.invalid_plate", "the plate cannot be sliced: an object is outside the printable area or the plate has errors "
                                                       "(see the notifications in the app; list_objects shows outside_print_volume)"};
            }
        }
        wxPostEvent(&p, SimpleEvent(all ? EVT_GLTOOLBAR_SLICE_ALL : EVT_GLTOOLBAR_SLICE_PLATE));
        wxGetApp().mainframe->select_tab(size_t(MainFrame::tpPreview));
        return json();
    });
    if (!wait) return json{{"started", true}, {"all_plates", all}};

    const auto t0      = std::chrono::steady_clock::now();
    bool       seen    = false;
    double     last_pg = -1;
    while (true) {
        json st;
        try {
            st = on_main([all]() {
            Plater        &p     = plater();
            PartPlateList &pl    = p.get_partplate_list();
            int            total = 0, done = 0;
            for (int i = 0; i < pl.get_plate_count(); ++i) {
                PartPlate *pp = pl.get_plate(i);
                if (!all && i != pl.get_curr_plate_index()) continue;
                if (pp->empty()) continue;
                ++total;
                if (pp->is_slice_result_valid()) ++done;
            }
            return json{{"slicing", p.is_background_process_slicing()}, {"error_plate", p.slice_error_plate()}, {"total", total}, {"done", done}};
            }, 30);
        } catch (const ToolError &e) {
            // Slicing/export failed and the app is showing its error dialog: report that as the slice result.
            if (e.code == "ui.blocked_by_dialog") {
                std::string what = e.message;
                if (auto i = what.find("dialog: "); i != std::string::npos) what = what.substr(i + 8);
                if (auto i = what.find(". "); i != std::string::npos) what = what.substr(0, i);
                throw ToolError{"slice.error", "slicing stopped with an error; the app shows it in a dialog: " + what +
                                                   " (read it with ui_dialog_inspect, close it with ui_close_dialog)"};
            }
            throw;
        }
        const bool slicing = st["slicing"].get<bool>();
        seen               = seen || slicing;
        int         completed, status;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(watch->m);
            completed = watch->completed;
            status    = watch->status;
            err       = watch->error;
        }
        const double el    = elapsed_s(t0);
        const int    total = st["total"].get<int>(), done = st["done"].get<int>();
        const double pg    = total > 0 ? 100.0 * done / total : 0;
        if (pg != last_pg) {
            ctx.progress(pg, "sliced " + std::to_string(done) + "/" + std::to_string(total) + " plate(s)");
            last_pg = pg;
        }
        if (status == int(core::slice_events::ProcessCompleted::Cancelled) && !slicing) throw ToolError{"slice.cancelled", "slicing was cancelled"};
        if (!slicing && st["error_plate"].get<int>() >= 0 && (seen || completed > 0))
            throw ToolError{"slice.error", err.empty() ? "slicing failed on plate " + std::to_string(st["error_plate"].get<int>() + 1) + " (see the app's notifications)" : err};
        if (!slicing && total > 0 && done == total && (seen || completed > 0 || el > 3.0)) break;
        // Nothing started: the background process had no work (e.g. the previous attempt failed during G-code export
        // and nothing changed since) or refused to start. Report it instead of waiting for the whole timeout.
        if (!slicing && !seen && completed == 0 && el > 8.0) {
            if (st["error_plate"].get<int>() >= 0)
                throw ToolError{"slice.error", "plate " + std::to_string(st["error_plate"].get<int>() + 1) +
                                                   " failed to slice earlier and nothing has changed since; fix the cause (see the app's notifications, "
                                                   "e.g. a G-code template error) and slice again"};
            throw ToolError{"slice.not_started", "slicing did not start (see the app's notifications for the reason)"};
        }
        if (el > timeout) throw ToolError{"slice.timeout", "slicing did not finish within " + std::to_string(int(timeout)) + " s (still running in the app)"};
        sleep_ms(300);
    }
    return on_main([all]() {
        json           out;
        PartPlateList &pl = plater().get_partplate_list();
        if (all) {
            json arr = json::array();
            for (int i = 0; i < pl.get_plate_count(); ++i)
                if (!pl.get_plate(i)->empty()) arr.push_back(plate_json(i));
            out["plates"] = arr;
        } else {
            out = plate_json(pl.get_curr_plate_index());
        }
        return out;
    });
}

// ---------------------------------------------------------------------------------------------
std::string to_hex(const unsigned char *md, unsigned n)
{
    static const char *digits = "0123456789abcdef";
    std::string        out;
    for (unsigned i = 0; i < n; ++i) {
        out += digits[md[i] >> 4];
        out += digits[md[i] & 15];
    }
    return out;
}

std::string sha256_of(const std::function<bool(EVP_MD_CTX *)> &feed)
{
    EVP_MD_CTX   *ctx = EVP_MD_CTX_new();
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned      n  = 0;
    bool          ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 && feed(ctx) && EVP_DigestFinal_ex(ctx, md, &n) == 1;
    if (ctx) EVP_MD_CTX_free(ctx);
    return ok ? to_hex(md, n) : std::string();
}

std::string sha256_hex(const std::string &data)
{
    return sha256_of([&](EVP_MD_CTX *c) { return EVP_DigestUpdate(c, data.data(), data.size()) == 1; });
}

std::string file_sha256(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (path.empty() || !f) return "missing";
    const std::string h = sha256_of([&](EVP_MD_CTX *c) {
        std::vector<char> buf(1 << 16);
        while (f) {
            f.read(buf.data(), std::streamsize(buf.size()));
            if (f.gcount() > 0 && EVP_DigestUpdate(c, buf.data(), size_t(f.gcount())) != 1) return false;
        }
        return true;
    });
    return h.empty() ? "unreadable" : h;
}

std::string config_digest(const DynamicPrintConfig &cfg)
{
    std::string ser;
    for (const std::string &k : cfg.keys()) ser += k + "=" + cfg.opt_serialize(k) + "\n";
    return sha256_hex(ser);
}

std::string jstr(const json &v) { return v.is_string() ? v.get<std::string>() : v.is_null() ? std::string() : v.dump(); }

// Test mode validates authorization without sending a print.
bool test_no_send()
{
    const char *e = std::getenv("PRINTBOT_MCP_TEST_NO_SEND");
    return e && std::string(e) == "1";
}

// w, or a window it belongs to, is a Print / Send dialog.
bool is_print_window(wxWindow *w)
{
    for (; w; w = w->GetParent())
        if (is_print_dialog(w)) return true;
    return false;
}

bool print_dialog_shown()
{
    for (auto *w : shown_dialogs())
        if (is_print_dialog(w)) return true;
    return false;
}

SelectMachineDialog *shown_select_machine_dialog()
{
    for (auto *w : shown_dialogs())
        if (auto *d = dynamic_cast<SelectMachineDialog *>(w)) return d;
    return nullptr;
}

void refuse_in_print_dialogs()
{
    if (print_dialog_shown())
        throw ToolError{"print.human_only",
                        "the Print / Send dialog is open: use print_plate with one explicit human print request, or cancel with ui_close_dialog."};
}

json printer_identity(MachineObject *o)
{
    if (!o) return nullptr;
    return json{{"dev_id", o->get_dev_id()}, {"name", o->get_dev_name()}, {"ip", o->get_dev_ip()}, {"model", o->get_show_printer_type()}};
}

std::string tray_label(MachineObject *o, const std::string &ams_id, const std::string &slot_id)
{
    if (!o) return {};
    for (const auto &entry : wxGetApp().sidebar().build_filament_ams_list(o)) {
        const auto &slot = entry.second;
        if (slot.opt_string("ams_id", 0u) == ams_id && slot.opt_string("slot_id", 0u) == slot_id) return slot.opt_string("tray_name", 0u);
    }
    return {};
}

// What the job is, independent of the Print dialog. Main thread.
json job_facts()
{
    Plater    &p  = plater();
    PartPlate *pp = p.get_partplate_list().get_curr_plate();
    json       j;
    j["project"]         = u8(p.get_project_name());
    j["path"]            = p.get_3mf_filename();
    j["plate"]           = p.get_partplate_list().get_curr_plate_index() + 1;
    j["plate_name"]      = pp->get_plate_name();
    j["sliced"]          = pp->is_slice_result_valid();
    j["ready_for_print"] = pp->is_slice_result_ready_for_print();
    json objects         = json::array();
    for (const auto &oi : pp->get_obj_and_inst_set()) {
        if (oi.first < 0 || oi.first >= int(p.model().objects.size())) continue;
        const ModelObject *object = p.model().objects[oi.first];
        if (oi.second < 0 || oi.second >= int(object->instances.size())) continue;
        objects.push_back({{"object_id", object->id().id}, {"name", object->name}, {"instance", oi.second}, {"printable", object->instances[oi.second]->printable}});
    }
    j["objects"] = objects;
    std::string gcode;
    if (GCodeProcessorResult *r = pp->get_slice_result(); r && pp->is_slice_result_valid()) gcode = r->filename;
    if (gcode.empty()) gcode = pp->get_tmp_gcode_path();
    j["gcode_sha256"]    = file_sha256(gcode);
    j["presets"]         = selected_presets_json();
    j["settings_sha256"] = config_digest(bundle().full_config());
    j["plate_settings_sha256"] = config_digest(*pp->config());
    DeviceManager *d = wxGetApp().getDeviceManager();
    j["printer"]     = printer_identity(d ? d->get_selected_machine() : nullptr);
    return j;
}

// What the Print dialog would send: destination, filament mapping, print options. Main thread.
json dialog_facts(SelectMachineDialog *dlg)
{
    json        j;
    j["printer"] = printer_identity(dlg->get_current_machine());
    std::string legacy, mapping, info;
    j["mapping_valid"]  = dlg->get_ams_mapping_result(legacy, mapping, info);
    j["mapping_legacy"] = legacy;
    j["mapping"]        = mapping;
    j["mapping_info"]   = info;
    j["options"]        = dlg->mcp_print_options();
    j["plate"]          = dlg->mcp_print_plate_idx() + 1;
    j["print_type"]     = int(dlg->get_print_type());
    return j;
}

std::string changed_parts(const json &a, const json &b)
{
    std::string out;
    for (const char *part : {"job", "dialog"})
        for (auto it = a[part].begin(); it != a[part].end(); ++it)
            if (!b[part].contains(it.key()) || b[part][it.key()] != it.value()) out += (out.empty() ? "" : ", ") + std::string(part) + "." + it.key();
    return out.empty() ? "unknown" : out;
}

// Printable job and destination summary.
std::string print_summary(const json &job, const json &dlg, const json &slice, MachineObject *m)
{
    std::string s;
    auto        line = [&](const std::string &k, const std::string &v) { s += k + std::string(k.size() < 16 ? 16 - k.size() : 1, ' ') + v + "\n"; };
    if (test_no_send()) s += "*** TEST MODE (PRINTBOT_MCP_TEST_NO_SEND=1): nothing will be sent ***\n\n";
    line("Project", jstr(job["project"]) + (jstr(job["path"]).empty() ? "" : "  (" + jstr(job["path"]) + ")"));
    line("Plate", std::to_string(job["plate"].get<int>()) + "  \"" + jstr(job["plate_name"]) + "\"");
    std::map<std::string, int> copies;
    std::vector<std::string>   order;
    for (const auto &o : job["objects"]) {
        if (!o["printable"].get<bool>()) continue;
        const std::string key = jstr(o["name"]) + " (id " + jstr(o["object_id"]) + ")";
        if (!copies.count(key)) order.push_back(key);
        ++copies[key];
    }
    std::string objs;
    for (const auto &k : order) objs += (objs.empty() ? "" : "\n                ") + k + "  x" + std::to_string(copies[k]);
    line("Objects", objs.empty() ? "(none)" : objs);
    const json &pr = dlg["printer"];
    line("Printer", pr.is_null() ? "(none selected in the Print dialog)"
                                 : jstr(pr["name"]) + "  model " + jstr(pr["model"]) + "  serial " + jstr(pr["dev_id"]) + "  IP " + jstr(pr["ip"]));
    const json &ps = job["presets"];
    line("Printer preset", jstr(ps["printer"]) + (ps.value("printer_dirty", false) ? " (modified)" : ""));
    line("Process", jstr(ps["process"]) + (ps.value("process_dirty", false) ? " (modified)" : ""));
    if (ps.contains("bed_type")) line("Bed type", jstr(ps["bed_type"]));
    // Filament mapping: project filament -> printer source.
    std::string maps;
    json        mapping = json::array(), info = json::array();
    try {
        if (!jstr(dlg["mapping"]).empty()) mapping = json::parse(jstr(dlg["mapping"]));
        if (!jstr(dlg["mapping_info"]).empty()) info = json::parse(jstr(dlg["mapping_info"]));
    } catch (...) {}
    const auto &fil = ps.contains("filaments") ? ps["filaments"] : json::array();
    for (size_t i = 0; i < mapping.size(); ++i) {
        const json       &mp   = mapping[i];
        const std::string ams  = mp.is_object() && mp.contains("ams_id") ? jstr(mp["ams_id"]) : "?";
        const std::string slot = mp.is_object() && mp.contains("slot_id") ? jstr(mp["slot_id"]) : "?";
        const std::string lbl  = tray_label(m, ams, slot);
        std::string       src  = (lbl.empty() ? "" : lbl + " ") + "(AMS " + ams + ", slot " + slot + ")";
        if (i < info.size() && info[i].is_object())
            src += "  " + jstr(info[i].value("filamentType", json(""))) + " colour " + jstr(info[i].value("targetColor", json("")));
        maps += (maps.empty() ? "" : "\n                ") + std::string("filament ") + std::to_string(i + 1) +
                (i < fil.size() ? " [" + jstr(fil[i]) + "]" : "") + " -> " + src;
    }
    line("Filaments", maps.empty() ? "(no mapping from the Print dialog)" : maps);
    if (!dlg.value("mapping_valid", false)) line("", "WARNING: the Print dialog reports the filament mapping as not valid");
    if (slice.value("sliced", false)) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s, %d layers, %.2f g / %.2f m filament", jstr(slice["print_time"]).c_str(), slice.value("layer_count", 0),
                 slice.value("filament_used_g", 0.0), slice.value("filament_used_m", 0.0));
        line("Estimate", buf);
        std::string warn;
        for (const auto &w : slice.value("warnings", json::array())) warn += (warn.empty() ? "" : "; ") + jstr(w.value("msg", json("")));
        if (!warn.empty()) line("Warnings", warn);
    }
    std::string opts;
    for (auto it = dlg["options"].begin(); it != dlg["options"].end(); ++it) opts += (opts.empty() ? "" : ", ") + it.key() + "=" + jstr(it.value());
    if (!opts.empty()) line("Options", opts);
    line("Copies", "one print of this plate (each object instance above is printed once)");
    return s;
}

struct SendOp
{
    std::string                           id;
    std::string                           fingerprint;
    std::string                           dev_id;
    std::string                           printer_name;
    std::string                           project;
    std::string                           plate_name;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    bool                                  test_mode = false;
    std::string                           final_check_error;
    wxWeakRef<SelectMachineDialog>        dialog;
    const void                           *prev_job = nullptr;
    std::shared_ptr<PrintJob>             job;
    std::string                           base_status, base_job_id, base_subtask_id, base_task_id, base_subtask_name;
};

std::mutex                                     g_ops_mutex;
std::map<std::string, std::shared_ptr<SendOp>> g_ops; // main thread writes, read under the mutex
std::string                                    g_last_op;
std::atomic<int>                               g_op_seq{0};

bool active_print_status(const std::string &s) { return s == "RUNNING" || s == "PREPARE" || s == "SLICING" || s == "PAUSE"; }

MachineObject *find_machine(const std::string &dev_id)
{
    DeviceManager *d = wxGetApp().getDeviceManager();
    if (!d || dev_id.empty()) return nullptr;
    for (auto &[id, m] : d->get_local_machinelist())
        if (m && id == dev_id) return m;
    return d->get_my_machine(dev_id);
}

// Main thread. Unknown stays unknown: a state is only reported when something shows it.
json op_status_json(SendOp &op)
{
    json j;
    j["operation_id"] = op.id;
    j["fingerprint"]  = op.fingerprint;
    j["printer"]      = {{"dev_id", op.dev_id}, {"name", op.printer_name}};
    j["age_s"]        = int(elapsed_s(op.created));
    if (op.test_mode) {
        j["status"] = "test_no_send";
        j["note"]   = "test instance (PRINTBOT_MCP_TEST_NO_SEND=1): approval and checks passed, Send was not pressed";
        return j;
    }
    if (!op.final_check_error.empty()) {
        j["status"] = "not_sent";
        j["reason"] = op.final_check_error;
        return j;
    }
    // Transfer, from the Print dialog's print job.
    if (!op.job && op.dialog) {
        auto cur = op.dialog->mcp_print_job();
        if (cur && cur.get() != op.prev_job) op.job = cur;
    }
    std::string transfer = "unknown";
    if (op.job) {
        if (op.job->is_finished()) transfer = "transferred";
        else if (op.job->is_running()) transfer = "uploading";
        else if (op.job->is_finalized()) transfer = "failed";
        else transfer = "starting";
        j["transfer_stage"] = op.job->get_print_stage();
    } else if (op.dialog) {
        bool confirm = false;
        for (auto *w : shown_dialogs())
            if (w != op.dialog.get() && is_print_window(w)) confirm = true;
        transfer = confirm ? "awaiting_user_confirmation" : elapsed_s(op.created) < 20 ? "send_requested" : "not_started";
    }
    j["transfer"] = transfer;
    // The printer's own report.
    std::string    printer_state = "unknown";
    MachineObject *o             = find_machine(op.dev_id);
    if (o && o->is_connected()) {
        const bool new_ids = (!o->job_id_.empty() && o->job_id_ != op.base_job_id && o->job_id_ != "0") ||
                             (!o->subtask_id_.empty() && o->subtask_id_ != op.base_subtask_id && o->subtask_id_ != "0") ||
                             (!o->task_id_.empty() && o->task_id_ != op.base_task_id && o->task_id_ != "0");
        const bool new_name   = !o->subtask_name.empty() && o->subtask_name != op.base_subtask_name;
        // The task name the print job sent (the user may rename it in the Print dialog), else the project / plate name.
        const std::string job_name   = op.job ? op.job->m_project_name : std::string();
        const bool        name_match = !o->subtask_name.empty() &&
                                ((!job_name.empty() && boost::iequals(o->subtask_name, job_name)) || (!op.project.empty() && boost::icontains(o->subtask_name, op.project)) ||
                                 (!op.plate_name.empty() && boost::icontains(o->subtask_name, op.plate_name)));
        const bool started    = !active_print_status(op.base_status) || new_ids || new_name;
        const bool matching   = started && name_match && (new_ids || new_name) && transfer == "transferred";
        if (matching) {
            const std::string &ps = o->print_status;
            printer_state = active_print_status(ps) ? (ps == "PAUSE" ? "paused" : "printing") : ps == "FAILED" ? "failed" : ps == "FINISH" ? "finished" : "unknown";
            j["printer_job"] = {{"task_name", o->subtask_name}, {"print_status", ps}, {"progress_pct", o->mc_print_percent},
                                {"matched_by", new_ids ? "new job id" : new_name ? "new task name" : "task name"}};
        } else {
            printer_state = "no_matching_job_reported";
        }
        j["printer_print_status"] = o->print_status;
    } else if (o) {
        printer_state = "printer_not_connected";
    }
    j["printer_state"] = printer_state;
    std::string status = transfer;
    if (printer_state == "printing" || printer_state == "paused" || printer_state == "finished") status = printer_state;
    else if (printer_state == "failed") status = "printer_failed";
    else if (transfer == "transferred") status = "transferred_awaiting_printer";
    j["status"] = status;
    return j;
}

std::shared_ptr<SendOp> find_op(const std::string &id)
{
    std::lock_guard<std::mutex> lk(g_ops_mutex);
    auto                        it = g_ops.find(id.empty() ? g_last_op : id);
    return it == g_ops.end() ? nullptr : it->second;
}

// Presses Send for the gate (the only caller allowed to touch a print dialog's buttons).
void gate_press(wxWindow *c)
{
    wxCommandEvent evt(wxEVT_BUTTON, c->GetId());
    evt.SetEventObject(c);
    c->GetEventHandler()->ProcessEvent(evt);
}

struct Waiter
{
    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
    bool                    ok   = false;
    std::string             code, message;
    void finish(bool ok_, std::string c = {}, std::string msg = {})
    {
        std::lock_guard<std::mutex> lk(m);
        if (done) return;
        ok      = ok_;
        code    = std::move(c);
        message = std::move(msg);
        done    = true;
        cv.notify_all();
    }
    bool wait(double timeout_s)
    {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(int(timeout_s * 1000)), [&] { return done; });
    }
};

// The single authorization gate for sending a print over MCP. The Print dialog must be open.
json gated_send(const json &pre, const std::string &authorization_id)
{
    const bool test = test_no_send();
    // 1. Freeze the job facts and build the summary the human will see.
    json snap = on_main([&]() {
        SelectMachineDialog *dlg = shown_select_machine_dialog();
        if (!dlg) throw ToolError{"print.no_dialog", "the Print dialog is not open"};
        json job = job_facts();
        if (sha256_hex(job.dump()) != pre["job_fingerprint"].get<std::string>())
            throw ToolError{"approval.stale", "the job changed while the Print dialog opened; run print_plate dry_run again and verify it still matches the human request"};
        json df = dialog_facts(dlg);
        if (!test) {
            const std::string want = pre.contains("printer") ? jstr(pre["printer"]["id"]) : std::string();
            if (df["printer"].is_null() || jstr(df["printer"]["dev_id"]) != want)
                throw ToolError{"approval.stale", "the Print dialog's destination printer is not the printer in the summary; nothing was sent"};
        }
        json facts = {{"job", job}, {"dialog", df}};
        return json{{"facts", facts},
                    {"fingerprint", sha256_hex(facts.dump())},
                    {"summary", print_summary(job, df, pre["job"]["slice"], dlg->get_current_machine())}};
    });
    const std::string fp = snap["fingerprint"].get<std::string>();

    auto op         = std::make_shared<SendOp>();
    op->id          = "print-" + std::to_string(++g_op_seq) + "-" + fp.substr(0, 8);
    op->fingerprint = fp;
    op->test_mode   = test;
    auto gate       = std::make_shared<Waiter>();
    post_to_main([gate, op, facts = snap["facts"], test, authorization_id]() {
        try {
            SelectMachineDialog *dlg = shown_select_machine_dialog();
            if (!dlg) return gate->finish(false, "print.no_dialog", "the Print dialog was closed after approval; nothing was sent");
            require_policy("Sending a print");
            const json now = {{"job", job_facts()}, {"dialog", dialog_facts(dlg)}};
            if (now != facts)
                return gate->finish(false, "approval.stale",
                                    "changed after approval (" + changed_parts(facts, now) + "); nothing was sent. Verify the changed job remains within the human request before retrying.");
            wxWindow *b = find_button(dlg, "Send");
            if (!test) {
                if (!b) return gate->finish(false, "print.no_button", "no Send button in the Print dialog; nothing was sent");
                if (!b->IsEnabled()) return gate->finish(false, "print.blocked", "Send is disabled in the Print dialog (see its message); nothing was sent");
            }
            if (!test && !facts["dialog"].value("mapping_valid", false))
                return gate->finish(false, "print.blocked", "filament mapping is invalid; nothing was sent");
            const auto authorization = printbot::mcp::consume_print_authorization(
                std::filesystem::path(Slic3r::data_dir()) / "mcp-print-authorizations", authorization_id);
            if (authorization == printbot::mcp::AuthorizationResult::reused)
                return gate->finish(false, "approval.used", "this human print request was already consumed; do not retry with a new ID");
            if (authorization != printbot::mcp::AuthorizationResult::consumed)
                return gate->finish(false, "approval.unavailable", "could not persist the single-use print authorization; nothing was sent");
            MachineObject *m   = dlg->get_current_machine();
            op->dev_id         = m ? m->get_dev_id() : std::string();
            op->printer_name   = m ? m->get_dev_name() : std::string();
            op->project        = jstr(facts["job"]["project"]);
            op->plate_name     = jstr(facts["job"]["plate_name"]);
            op->dialog         = dlg;
            op->prev_job       = dlg->mcp_print_job().get();
            if (m) {
                op->base_status       = m->print_status;
                op->base_job_id       = m->job_id_;
                op->base_subtask_id   = m->subtask_id_;
                op->base_task_id      = m->task_id_;
                op->base_subtask_name = m->subtask_name;
            }
            {
                std::lock_guard<std::mutex> lk(g_ops_mutex);
                g_ops[op->id] = op;
                g_last_op     = op->id;
            }
            // The same check runs again inside the Print dialog immediately before transport starts.
            auto final_check = [facts, dlg, op](std::string &why) {
                try {
                    require_policy("Sending a print");
                    const json current = {{"job", job_facts()}, {"dialog", dialog_facts(dlg)}};
                    if (current == facts) return true;
                    why = "the approved job changed before transport (" + changed_parts(facts, current) + ")";
                } catch (const ToolError &e) {
                    why = e.code + ": " + e.message;
                } catch (...) {
                    why = "final authorization check failed";
                }
                op->final_check_error = why;
                return false;
            };
            if (test) {
                std::string why;
                if (!final_check(why)) return gate->finish(false, "approval.stale", why + "; nothing was sent");
            } else {
                dlg->mcp_arm_final_check(final_check, std::chrono::steady_clock::now() + std::chrono::minutes(10));
            }
            // Answer before pressing: Send may open further confirmations that only the user can answer.
            gate->finish(true);
            if (test) {
                BOOST_LOG_TRIVIAL(warning) << "print_bot MCP: test mode, approved job " << op->id << " passed the gate; Send NOT pressed";
                return;
            }
            BOOST_LOG_TRIVIAL(info) << "print_bot MCP: approved job " << op->id << " passed the gate; pressing Send";
            gate_press(b);
        } catch (const ToolError &e) {
            gate->finish(false, e.code, e.message);
        } catch (const std::exception &e) {
            gate->finish(false, "internal", e.what());
        }
    });
    if (!gate->wait(60)) throw ToolError{"print.unknown", "the app did not confirm the send step; check get_print_operation and the Print dialog before anything else"};
    if (!gate->ok) throw ToolError{gate->code, gate->message};

    sleep_ms(2500);
    json st;
    try {
        st = on_main([op]() { return op_status_json(*op); }, 20);
    } catch (const ToolError &) {
        st = json{{"operation_id", op->id}, {"status", "send_requested"}};
    }
    return json{{"status", st.value("status", std::string("send_requested"))},
                {"operation_id", op->id},
                {"approved_fingerprint", fp},
                {"authorization_id", authorization_id},
                {"summary", snap["summary"]},
                {"operation", st},
                {"preflight", pre},
                {"note", "Send was requested after the user's approval. This is not proof that the printer accepted the job: poll "
                         "get_print_operation until it reports printing, or a failure. Never resend on an unknown status; ask the user."}};
}

// ---------------------------------------------------------------------------------------------
// status

json ui_state_json()
{
    MainFrame *mf      = wxGetApp().mainframe;
    Plater    &p       = plater();
    json       j;
    j["tab"]           = mf->m_tabpanel ? tab_name(mf->m_tabpanel->GetSelection()) : "unknown";
    j["view"]          = p.is_preview_shown() ? "preview" : p.is_view3D_shown() ? "3d" : "other";
    json             sel = json::array();
    const Selection &s   = p.canvas3D()->get_selection();
    std::set<int>    obj_idxs;
    for (unsigned vi : s.get_volume_idxs())
        if (const GLVolume *v = s.get_volume(vi)) obj_idxs.insert(v->object_idx());
    for (int idx : obj_idxs)
        if (idx >= 0 && idx < int(p.model().objects.size())) sel.push_back(p.model().objects[idx]->id().id);
    j["selected_object_ids"] = sel;
    json dl                  = json::array();
    auto dlgs                = shown_dialogs();
    for (size_t i = 0; i < dlgs.size(); ++i) dl.push_back(dialog_json(dlgs[i], i));
    j["dialogs"]     = dl;
    j["window_size"] = {mf->GetSize().GetWidth(), mf->GetSize().GetHeight()};
    j["dark_mode"]   = wxGetApp().dark_mode();
    return j;
}

json status_json(ToolContext &ctx)
{
    const uint64_t seq = ctx.core.events().last_seq();
    return on_main([seq]() {
        Plater &p    = plater();
        json    j;
        j["mode"]    = "live";
        j["server"]  = {{"name", "print_bot"}, {"core_version", core::Core::version()}, {"slicer_version", SLIC3R_VERSION}};
        j["project"] = {{"name", u8(p.get_project_name())},
                        {"path", p.get_3mf_filename()},
                        {"dirty", p.is_project_dirty()},
                        {"objects", p.model().objects.size()},
                        {"plates", p.get_partplate_list().get_plate_count()},
                        {"current_plate", p.get_partplate_list().get_curr_plate_index() + 1}};
        j["presets"]                 = selected_presets_json();
        j["current_plate"]           = plate_json(p.get_partplate_list().get_curr_plate_index());
        j["slicing"]                 = p.is_background_process_slicing();
        j["ui"]                      = ui_state_json();
        j["printer_control_allowed"] = printer_control_allowed();
        j["approval_required_before_every_print"] = true;
        j["print_approval"] = "one explicit human chat request authorizes one print attempt; no approval popup";
        if (test_no_send()) j["test_no_send"] = true;
        if (DeviceManager *d = wxGetApp().getDeviceManager())
            if (MachineObject *o = d->get_selected_machine()) j["selected_printer"] = printer_brief(o, o);
        j["last_event_seq"] = seq;
        return j;
    });
}

// ---------------------------------------------------------------------------------------------
// registration

struct Reg
{
    std::vector<Tool> &t;
    void operator()(std::string name, std::string title, std::string desc, json schema, bool read_only, std::function<json(ToolContext &, const json &)> h)
    {
        if (!read_only) {
            auto inner = std::move(h);
            h          = [title, inner](ToolContext &ctx, const json &a) {
                agent_activity(title);
                return inner(ctx, a);
            };
        }
        // Read-only tools, the dialog tools (they answer the dialog another call is waiting in) and
        // the print tools (own gate, may wait on their own dialog) may run while a modal dialog is
        // shown; every other tool is refused instead of running nested in that dialog's event loop.
        static const std::set<std::string> nest_ok{"ui_list_dialogs", "ui_dialog_inspect", "ui_dialog_click", "ui_dialog_set",
                                                   "ui_close_dialog",  "ui_screenshot",     "print_plate",     "get_print_operation",
                                                   "printer_control"};
        const bool may_nest = read_only || nest_ok.count(name) > 0;
        {
            auto inner = std::move(h);
            h          = [may_nest, inner](ToolContext &ctx, const json &a) {
                set_tool_may_nest(may_nest);
                return inner(ctx, a);
            };
        }
        t.push_back(Tool{std::move(name), std::move(title), std::move(desc), std::move(schema), read_only, std::move(h)});
    }
};

json arr_of(const char *type, const std::string &desc) { return json{{"type", "array"}, {"items", {{"type", type}}}, {"description", desc}}; }

} // namespace

std::string gui_instructions()
{
    return "print_bot is a LAN-only Bambu Lab slicer. You are connected LIVE to the running print_bot window: every tool acts on "
           "the app the user is looking at, through the same paths as their clicks, and they see each change. "
           "Typical flow: get_status -> select_presets -> load_model / open_project -> arrange_objects -> slice -> export_gcode, "
           "or print_plate (only when the user enabled printer control). Objects are addressed by id (list_objects), plates by "
           "1-based number. If a call returns ui.blocked_by_dialog, a dialog is waiting: inspect it with ui_list_dialogs / "
           "ui_dialog_inspect and answer with ui_dialog_click (or ask the user). ui_screenshot saves a PNG of the 3D view or the "
           "window so you can check the result. get_events polls state-change events. " + printbot::mcp::print_approval_instructions();
}

std::vector<Tool> make_gui_tools()
{
    std::vector<Tool> tools;
    Reg               add{tools};
    const json        kPlateProp   = prop("integer", "Plate number (1-based, as labelled in the app). Default: the current plate.");
    const json        kObjectProp  = prop("integer", "Object id (see list_objects).");
    const json        kPrinterProp = prop("string", "Printer serial, name or IP from list_printers. Default: the selected printer.");
    const json        kTypeProp    = enum_prop({"printer", "process", "filament"}, "Preset type.");

    // ---------------- general ----------------
    add("get_status", "Get status",
        "Live app state: project (name, path, dirty, objects, plates), selected presets, current plate with slice summary, "
        "UI state (tab, view, selection, open dialogs), selected printer, and whether printer control is allowed.",
        schema(), true, [](ToolContext &ctx, const json &) { return status_json(ctx); });

    add("get_version", "Get version", "Versions of the print_bot core API and the slicing engine.", schema(), true, [](ToolContext &, const json &) {
        return json{{"print_bot_core", core::Core::version()}, {"slicer_engine", SLIC3R_VERSION}, {"mode", "live"}};
    });

    add("get_events", "Get events",
        "Core state-change events after a sequence number (slice.process_completed, slice.export_finished, ...). Poll with since = last_seq.",
        schema({{"since", prop("integer", "Return events with seq > since (default 0).")}, {"limit", prop("integer", "Max events (default 100).")}}), true,
        [](ToolContext &ctx, const json &a) {
            auto evs = ctx.core.events().history_since(uint64_t(std::max(0.0, num_arg(a, "since", 0))), size_t(std::clamp(num_arg(a, "limit", 100), 1.0, 500.0)));
            json arr = json::array();
            for (const auto &e : evs) arr.push_back(json{{"seq", e.seq}, {"topic", core::topic_name(e.topic)}, {"key", e.key}, {"detail", e.detail}});
            return json{{"events", arr}, {"last_seq", ctx.core.events().last_seq()}};
        });

    // ---------------- presets ----------------
    const json filter_props = {{"filter", prop("string", "Case-insensitive substring of the preset name.")},
                               {"visible_only", prop("boolean", "Only presets enabled in 'Printers and filaments'. Default false.")},
                               {"limit", prop("integer", "Max presets returned (default 300).")}};
    json compat_props               = filter_props;
    compat_props["compatible_only"] = prop("boolean", "Only presets compatible with the selected printer. Default true.");
    add("list_printer_presets", "List printer presets", "Printer presets (one per printer model and nozzle).", schema(filter_props), true,
        [](ToolContext &, const json &a) { return list_presets(a, Preset::TYPE_PRINTER); });
    add("list_process_presets", "List process presets", "Process (quality) presets, by default only those compatible with the printer.",
        schema(compat_props), true, [](ToolContext &, const json &a) { return list_presets(a, Preset::TYPE_PRINT); });
    add("list_filament_presets", "List filament presets", "Filament presets, by default only those compatible with the printer.", schema(compat_props),
        true, [](ToolContext &, const json &a) { return list_presets(a, Preset::TYPE_FILAMENT); });

    add("get_selected_presets", "Get selected presets",
        "Selected printer, process, filament slots and bed type, with unsaved-changes flags and the available bed types.", schema(), true,
        [](ToolContext &, const json &) {
            return on_main([]() {
                json j                   = selected_presets_json();
                j["available_bed_types"] = print_config_def.get("curr_bed_type")->enum_values;
                return j;
            });
        });

    add("select_presets", "Select presets",
        "Select presets in the sidebar, exactly like choosing them in the combo boxes (the printer first). 'filaments' sets the filament "
        "slots in order and adds/removes slots to match its length. If presets have unsaved changes the app may ask what to do "
        "(ui.blocked_by_dialog).",
        schema({{"printer", prop("string", "Printer preset name, e.g. 'Bambu Lab P2S 0.4 nozzle'.")},
                {"process", prop("string", "Process preset name, e.g. '0.20mm Standard @BBL P2S'.")},
                {"filaments", arr_of("string", "Filament preset names, slot 1 first.")},
                {"bed_type", prop("string", "Bed type, e.g. 'Textured PEI Plate' (see get_selected_presets).")}}),
        false, [](ToolContext &, const json &a) {
            auto printer   = opt_str(a, "printer");
            auto process   = opt_str(a, "process");
            auto filaments = str_list_arg(a, "filaments");
            auto bed       = opt_str(a, "bed_type");
            if (printer)
                on_main([n = *printer]() {
                    select_printer_ui(resolve_preset(Preset::TYPE_PRINTER, n));
                    return json();
                });
            if (process)
                on_main([n = *process]() {
                    const std::string name = resolve_preset(Preset::TYPE_PRINT, n);
                    if (bundle().prints.get_selected_preset_name() != name) {
                        tab_for(Preset::TYPE_PRINT)->select_preset(name);
                        plater().on_config_change(bundle().full_config());
                    }
                    return json();
                });
            if (!filaments.empty())
                on_main([filaments]() {
                    std::vector<std::string> names;
                    for (const auto &f : filaments) names.push_back(resolve_preset(Preset::TYPE_FILAMENT, f));
                    Sidebar &sb = plater().sidebar();
                    while (bundle().filament_presets.size() < names.size()) {
                        const size_t before = bundle().filament_presets.size();
                        sb.add_filament();
                        if (bundle().filament_presets.size() == before) throw ToolError{"preset.filament_slots", "cannot add more filament slots"};
                    }
                    while (bundle().filament_presets.size() > names.size()) sb.delete_filament(bundle().filament_presets.size() - 1);
                    for (size_t i = 0; i < names.size(); ++i) set_filament_slot_ui(i, names[i]);
                    return json();
                });
            if (bed)
                on_main([b = *bed]() {
                    if (!plater().sidebar().set_bed_type(b)) bad_arg("unknown bed type '" + b + "' (see get_selected_presets)");
                    return json();
                });
            return on_main([]() { return selected_presets_json(); });
        });

    add("get_preset_settings", "Get preset settings",
        "Read options of the selected (edited) printer, process or filament preset, including unsaved edits. Give 'keys' to read "
        "specific options (e.g. layer_height, sparse_infill_density, wall_loops, nozzle_temperature); without keys returns the options "
        "with unsaved edits plus a few common ones.",
        schema({{"type", kTypeProp}, {"keys", arr_of("string", "Option keys to read.")}, {"all", prop("boolean", "Return every option (large). Default false.")}},
               {"type"}),
        true, [](ToolContext &, const json &a) {
            const Preset::Type t    = type_arg(a);
            auto               keys = str_list_arg(a, "keys");
            const bool         all  = bool_arg(a, "all", false);
            return on_main([=]() {
                PresetCollection        &c      = collection(t);
                const Preset            &edited = c.get_edited_preset();
                json                     vals   = json::object();
                std::vector<std::string> ks     = keys;
                if (all) ks = edited.config.keys();
                else if (ks.empty()) {
                    ks = c.current_dirty_options();
                    for (const char *k : {"layer_height", "wall_loops", "sparse_infill_density", "sparse_infill_pattern", "enable_support", "support_type",
                                          "brim_type", "nozzle_temperature", "hot_plate_temp", "filament_type", "nozzle_diameter", "printer_model"})
                        if (edited.config.has(k)) ks.push_back(k);
                }
                json unknown = json::array();
                for (const auto &k : ks) {
                    if (edited.config.has(k)) vals[k] = edited.config.opt_serialize(k);
                    else unknown.push_back(k);
                }
                json j = {{"type", type_name(t)}, {"preset", edited.name}, {"dirty", c.current_is_dirty()}, {"dirty_options", c.current_dirty_options()}, {"values", vals}};
                if (!unknown.empty()) j["unknown_keys"] = unknown;
                return j;
            });
        });

    add("set_preset_settings", "Change preset settings",
        "Edit options of the selected printer, process or filament preset, like typing into the settings panel (the change shows as an "
        "unsaved edit and the plate needs re-slicing). Values use the config text format, e.g. {\"layer_height\": \"0.16\", "
        "\"sparse_infill_density\": \"25%\", \"wall_loops\": \"3\"}. Filament edits apply to the filament shown in the filament settings "
        "(get_selected_presets.edited_filament). Use save_preset to keep the changes as a user preset.",
        schema({{"type", kTypeProp}, {"values", json{{"type", "object"}, {"description", "Option key -> value (string or number)."}}}}, {"type", "values"}),
        false, [](ToolContext &, const json &a) {
            const Preset::Type t = type_arg(a);
            if (!a.contains("values") || !a["values"].is_object() || a["values"].empty()) bad_arg("'values' must be a non-empty object");
            json values = a["values"];
            return on_main([=]() {
                Tab                      *tab = tab_for(t);
                DynamicPrintConfig        cfg = collection(t).get_edited_preset().config;
                ConfigSubstitutionContext sctx(ForwardCompatibilitySubstitutionRule::Disable);
                for (auto it = values.begin(); it != values.end(); ++it) {
                    const std::string key = it.key();
                    if (!cfg.has(key)) throw ToolError{"preset.unknown_option", "'" + key + "' is not a " + std::string(type_name(t)) + " option"};
                    const std::string v = it->is_string() ? it->get<std::string>() : it->dump();
                    bool              ok = false;
                    try {
                        ok = cfg.set_deserialize_nothrow(key, v, sctx);
                    } catch (const std::exception &e) {
                        throw ToolError{"preset.bad_value", key + ": " + e.what()};
                    }
                    if (!ok) throw ToolError{"preset.bad_value", "invalid value '" + v + "' for " + key};
                    check_option_range(cfg, key, "preset.bad_value");
                }
                tab->load_config(cfg);
                plater().on_config_change(bundle().full_config());
                PresetCollection &c = collection(t);
                return json{{"preset", c.get_edited_preset().name}, {"dirty", c.current_is_dirty()}, {"dirty_options", c.current_dirty_options()}};
            });
        });

    add("save_preset", "Save preset",
        "Save the selected printer/process/filament preset with its edits as a user preset named 'name' (like the Save button; an "
        "existing user preset with that name is overwritten). The new preset becomes selected.",
        schema({{"type", kTypeProp}, {"name", prop("string", "User preset name.")}}, {"type", "name"}), false, [](ToolContext &, const json &a) {
            const Preset::Type t    = type_arg(a);
            const std::string  name = str_arg(a, "name", true);
            if (name.empty()) bad_arg("'name' is empty");
            return on_main([=]() {
                PresetCollection &c = collection(t);
                if (const Preset *ex = c.find_preset(name, false); ex && ex->is_system) throw ToolError{"preset.system", "'" + name + "' is a system preset name"};
                tab_for(t)->save_preset(name);
                return json{{"saved", c.get_selected_preset_name()}, {"dirty", c.current_is_dirty()}};
            });
        });

    add("discard_preset_changes", "Discard preset changes", "Drop unsaved edits of the selected printer/process/filament preset.",
        schema({{"type", kTypeProp}}, {"type"}), false, [](ToolContext &, const json &a) {
            const Preset::Type t = type_arg(a);
            return on_main([=]() {
                PresetCollection &c = collection(t);
                c.discard_current_changes();
                tab_for(t)->load_current_preset();
                plater().on_config_change(bundle().full_config());
                return json{{"preset", c.get_edited_preset().name}, {"dirty", c.current_is_dirty()}};
            });
        });

    // ---------------- project ----------------
    add("new_project", "New project", "Start a new empty project.",
        schema({{"discard_unsaved", prop("boolean", "Don't ask about unsaved changes. Default false (the app may ask: ui.blocked_by_dialog).")}}), false,
        [](ToolContext &, const json &a) {
            const bool discard = bool_arg(a, "discard_unsaved", false);
            return on_main([=]() {
                Plater &p = plater();
                p.new_project(discard, discard);
                wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                return json{{"objects", p.model().objects.size()}, {"plates", p.get_partplate_list().get_plate_count()}};
            });
        });

    add("open_project", "Open project",
        "Open a .3mf project like File > Open project: its plates, objects and settings (printer/process/filament presets, per-object "
        "settings) replace the current project.",
        schema({{"path", prop("string", "Absolute path of the .3mf file.")},
                {"discard_unsaved", prop("boolean", "Don't ask about unsaved changes in the current project. Default false.")}},
               {"path"}),
        false, [](ToolContext &, const json &a) {
            const std::string path    = str_arg(a, "path", true);
            const bool        discard = bool_arg(a, "discard_unsaved", false);
            if (!fs::exists(path)) bad_arg("file not found: " + path);
            return on_main([=]() {
                Plater &p = plater();
                if (discard) p.set_plater_dirty(false);
                wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                int rc = p.load_project(wx(path));
                if (rc == wxID_CANCEL) throw ToolError{"project.cancelled", "opening the project was cancelled"};
                return json{{"project", u8(p.get_project_name())}, {"objects", objects_json()}, {"plates", plates_json()}, {"presets", selected_presets_json()}};
            }, 600);
        });

    add("save_project", "Save project", "Save the project as a .3mf file (without dialogs).",
        schema({{"path", prop("string", "Absolute .3mf path. Default: the project's current file.")}}), false, [](ToolContext &, const json &a) {
            const std::string path = a.contains("path") ? check_out_path(a, "path", ".3mf") : std::string();
            return on_main([=]() {
                Plater     &p    = plater();
                std::string dest = path.empty() ? u8(p.get_project_filename(".3mf")) : path;
                if (dest.empty()) throw ToolError{"project.no_path", "the project has no file yet; pass 'path'"};
                if (p.save_project_to(wx(dest)) != wxID_YES) throw ToolError{"project.save_failed", "could not save " + dest};
                return json{{"path", dest}, {"bytes", fs::exists(dest) ? fs::file_size(dest) : 0}};
            }, 600);
        });

    add("load_model", "Load model",
        "Import model files (STL, 3MF, OBJ, STEP, AMF, SVG) into the current project, like File > Import. With a 3MF its settings "
        "are loaded too unless load_settings is false. Returns the new objects.",
        schema({{"path", prop("string", "Absolute path of a model file.")},
                {"paths", arr_of("string", "Several files at once.")},
                {"append", prop("boolean", "Keep the existing objects (default true). false = start a new project first.")},
                {"load_settings", prop("boolean", "For 3MF: also load its presets/settings (default true).")}}),
        false, [](ToolContext &, const json &a) {
            std::vector<std::string> paths = str_list_arg(a, "paths");
            if (auto p = opt_str(a, "path")) paths.insert(paths.begin(), *p);
            if (paths.empty()) bad_arg("give 'path' or 'paths'");
            for (const auto &p : paths)
                if (!fs::exists(p)) bad_arg("file not found: " + p);
            const bool append   = bool_arg(a, "append", true);
            const bool settings = bool_arg(a, "load_settings", true);
            return on_main([=]() {
                Plater &p = plater();
                if (!append) p.new_project(true, true);
                wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                std::vector<uint64_t> before;
                for (auto *o : p.model().objects) before.push_back(o->id().id);
                std::vector<fs::path> fps(paths.begin(), paths.end());
                LoadStrategy          strategy = settings ? LoadStrategy::LoadModel | LoadStrategy::LoadConfig : LoadStrategy::LoadModel;
                p.load_files(fps, strategy, false);
                json added = json::array();
                for (size_t i = 0; i < p.model().objects.size(); ++i)
                    if (std::find(before.begin(), before.end(), p.model().objects[i]->id().id) == before.end()) added.push_back(object_json(i));
                if (added.empty()) throw ToolError{"model.load_failed", "no objects were loaded (see the app's notifications)"};
                return json{{"added", added}, {"objects", p.model().objects.size()}};
            }, 600);
        });

    add("undo", "Undo", "Undo the last change (like Ctrl+Z).", schema(), false, [](ToolContext &, const json &) {
        return on_main([]() {
            Plater &p = plater();
            if (!p.can_undo()) throw ToolError{"undo.empty", "nothing to undo"};
            p.undo();
            return json{{"objects", objects_json()}};
        });
    });
    add("redo", "Redo", "Redo the last undone change (like Ctrl+Y).", schema(), false, [](ToolContext &, const json &) {
        return on_main([]() {
            Plater &p = plater();
            if (!p.can_redo()) throw ToolError{"redo.empty", "nothing to redo"};
            p.redo();
            return json{{"objects", objects_json()}};
        });
    });

    // ---------------- objects ----------------
    add("list_objects", "List objects",
        "All objects with id, name, plate, position/rotation/scale of the first instance, size, filament slot, per-object settings, "
        "and whether they are outside the printable volume.",
        schema(), true, [](ToolContext &, const json &) { return on_main([]() { return json{{"objects", objects_json()}}; }); });

    add("remove_object", "Remove object", "Delete objects by id (like selecting them and pressing Delete).",
        schema({{"id", kObjectProp}, {"ids", arr_of("integer", "Several object ids.")}}), false, [](ToolContext &, const json &a) {
            auto ids = int_list_arg(a, "ids");
            if (a.contains("id")) ids.push_back(int_arg(a, "id"));
            if (ids.empty()) bad_arg("give 'id' or 'ids'");
            return on_main([ids]() {
                std::vector<int> idx;
                for (auto id : ids) idx.push_back(object_index(uint64_t(id)));
                if (!plater().is_view3D_shown()) wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                select_objects_ui(idx);
                plater().remove_selected();
                return json{{"objects", objects_json()}};
            });
        });

    add("clear_project", "Delete all objects", "Delete all objects (plates and settings stay).", schema(), false, [](ToolContext &, const json &) {
        return on_main([]() {
            plater().delete_all_objects_from_model();
            return json{{"objects", objects_json()}};
        });
    });

    add("select_objects", "Select objects", "Select objects in the 3D view and object list (UI selection, like clicking them). Empty = clear.",
        schema({{"ids", arr_of("integer", "Object ids.")}, {"all", prop("boolean", "Select all objects.")}}), false, [](ToolContext &, const json &a) {
            auto       ids = int_list_arg(a, "ids");
            const bool all = bool_arg(a, "all", false);
            return on_main([=]() {
                if (all) plater().select_all();
                else {
                    std::vector<int> idx;
                    for (auto id : ids) idx.push_back(object_index(uint64_t(id)));
                    select_objects_ui(idx);
                }
                return ui_state_json();
            });
        });

    add("transform_object", "Move / rotate / scale object",
        "Transform an object like the move/rotate/scale fields in the sidebar (undoable; the plate is re-assigned and the object stays "
        "on the bed). position = absolute [x,y] (or [x,y,z]) of the bounding-box center in mm, bed coordinates; move_by = relative "
        "offset in mm; rotate_deg = relative rotation [rx,ry,rz] in degrees around the world axes; scale = absolute factor (number = "
        "uniform, or [sx,sy,sz], along the object's own axes); size_mm = target [x,y,z] size along the object's own axes. Applied in the order rotate, scale/size, move.",
        schema({{"id", kObjectProp},
                {"position", arr_of("number", "[x, y] or [x, y, z] in mm.")},
                {"move_by", arr_of("number", "[dx, dy] or [dx, dy, dz] in mm.")},
                {"rotate_deg", arr_of("number", "[rx, ry, rz] in degrees.")},
                {"scale", json{{"description", "Number or [sx, sy, sz]; 1 = original size."}}},
                {"size_mm", arr_of("number", "[x, y, z] in mm.")}},
               {"id"}),
        false, [](ToolContext &, const json &a) {
            const uint64_t id = uint64_t(int_arg(a, "id", true));
            auto           v3 = [&](const char *k, size_t min_n) -> std::optional<std::vector<double>> {
                if (!a.contains(k) || a[k].is_null()) return std::nullopt;
                if (!a[k].is_array() || a[k].size() < min_n || a[k].size() > 3)
                    bad_arg(std::string("'") + k + "' must be an array of " + std::to_string(min_n) + " to 3 numbers");
                std::vector<double> v;
                for (auto &x : a[k]) {
                    if (!x.is_number()) bad_arg(std::string("'") + k + "' must contain numbers");
                    v.push_back(x.get<double>());
                }
                return v;
            };
            auto position = v3("position", 2), move_by = v3("move_by", 2), rotate = v3("rotate_deg", 3), size = v3("size_mm", 3);
            std::optional<Vec3d> scale;
            if (a.contains("scale") && !a["scale"].is_null()) {
                if (a["scale"].is_number()) {
                    const double s = a["scale"].get<double>();
                    scale          = Vec3d(s, s, s);
                } else if (auto s = v3("scale", 3))
                    scale = Vec3d((*s)[0], (*s)[1], (*s)[2]);
            }
            if (scale && scale->minCoeff() <= 0) bad_arg("'scale' must be positive");
            if (size && *std::min_element(size->begin(), size->end()) <= 0) bad_arg("'size_mm' must be positive");
            // Keep geometry within the slicer's 32-bit coordinate range (about +-21 m); beyond that the clipper overflows.
            auto too_far = [](const std::optional<std::vector<double>> &v, double lim) {
                if (v) for (double x : *v) if (!std::isfinite(x) || std::abs(x) > lim) return true;
                return false;
            };
            if (too_far(position, 5000.) || too_far(move_by, 5000.)) bad_arg("'position'/'move_by' must be within +-5000 mm");
            if (too_far(size, 5000.)) bad_arg("'size_mm' must be at most 5000 mm");
            if (scale && (!scale->allFinite() || scale->maxCoeff() > 1000.)) bad_arg("'scale' must be at most 1000");
            return on_main([=]() {
                Plater     &p      = plater();
                const int   idx    = object_index(id);
                GLCanvas3D *canvas = p.canvas3D();
                if (!p.is_view3D_shown()) wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                select_objects_ui({idx});
                Selection   &sel = canvas->get_selection();
                ModelObject *o   = p.model().objects[idx];
                if (rotate) {
                    sel.setup_cache();
                    sel.rotate(Vec3d((*rotate)[0], (*rotate)[1], (*rotate)[2]) * (M_PI / 180.0), TransformationType(TransformationType::World_Relative_Joint));
                    canvas->do_rotate("Set Rotation");
                }
                if (scale || size) {
                    // Same as the sidebar's scale/size fields in object (instance) coordinates: absolute factors
                    // relative to the unscaled object.
                    sel.setup_cache();
                    Vec3d target = scale ? *scale : o->instances[0]->get_scaling_factor();
                    if (size) {
                        const Vec3d ref = sel.get_full_unscaled_instance_local_bounding_box().size();
                        for (int k = 0; k < 3; ++k)
                            if (ref[k] > 1e-6) target[k] = (*size)[k] / ref[k];
                    }
                    // Selection converts "absolute" factors through the sidebar's current reference system, so pass
                    // a relative factor in instance coordinates (exact for rotated objects too).
                    const Vec3d cur = o->instances[0]->get_scaling_factor();
                    Vec3d       rel = Vec3d::Ones();
                    for (int k = 0; k < 3; ++k)
                        if (std::abs(cur[k]) > 1e-9) rel[k] = target[k] / cur[k];
                    TransformationType tt;
                    tt.set_instance();
                    tt.set_relative();
                    sel.scale(rel, tt);
                    canvas->do_scale("Set Scale");
                }
                if (position || move_by) {
                    Vec3d disp = Vec3d::Zero();
                    if (position) {
                        const Vec3d c = o->instance_bounding_box(0).center();
                        disp          = Vec3d((*position)[0] - c.x(), (*position)[1] - c.y(), position->size() > 2 ? (*position)[2] - c.z() : 0.0);
                    }
                    if (move_by) disp += Vec3d((*move_by)[0], (*move_by)[1], move_by->size() > 2 ? (*move_by)[2] : 0.0);
                    sel.setup_cache();
                    sel.translate(disp, TransformationType(TransformationType::World_Relative_Joint));
                    canvas->do_move("Set Position");
                }
                return object_json(size_t(object_index(id)));
            });
        });

    add("set_object_settings", "Set per-object settings",
        "Override process settings for one object (like adding settings to an object in the object list), e.g. "
        "{\"sparse_infill_density\": \"40%\", \"wall_loops\": \"4\"}. A null value removes that override.",
        schema({{"id", kObjectProp}, {"values", json{{"type", "object"}, {"description", "Option key -> value; null removes."}}}}, {"id", "values"}), false,
        [](ToolContext &, const json &a) {
            const uint64_t id = uint64_t(int_arg(a, "id", true));
            if (!a.contains("values") || !a["values"].is_object()) bad_arg("'values' must be an object");
            json values = a["values"];
            return on_main([=]() {
                Plater      &p   = plater();
                const int    idx = object_index(id);
                ModelObject *o   = p.model().objects[idx];
                select_objects_ui({idx});
                p.take_snapshot("Change object settings");
                const DynamicPrintConfig &proc = bundle().prints.get_edited_preset().config;
                ConfigSubstitutionContext sctx(ForwardCompatibilitySubstitutionRule::Disable);
                for (auto it = values.begin(); it != values.end(); ++it) {
                    const std::string key = it.key();
                    if (it->is_null()) {
                        o->config.erase(key);
                        continue;
                    }
                    if (!proc.has(key)) throw ToolError{"object.unknown_option", "'" + key + "' is not a process option"};
                    DynamicPrintConfig tmp;
                    const std::string  v = it->is_string() ? it->get<std::string>() : it->dump();
                    if (!tmp.set_deserialize_nothrow(key, v, sctx)) throw ToolError{"object.bad_value", "invalid value '" + v + "' for " + key};
                    check_option_range(tmp, key, "object.bad_value");
                    o->config.set_key_value(key, tmp.option(key)->clone());
                }
                wxGetApp().obj_list()->update_and_show_object_settings_item();
                wxGetApp().obj_list()->changed_object(idx);
                p.update();
                return object_json(size_t(idx));
            });
        });

    add("set_object_filament", "Set object filament", "Print an object with filament slot N (1-based), like the object list's filament column.",
        schema({{"id", kObjectProp}, {"filament", prop("integer", "Filament slot (1-based).")}}, {"id", "filament"}), false, [](ToolContext &, const json &a) {
            const uint64_t id = uint64_t(int_arg(a, "id", true));
            const int      f  = int(int_arg(a, "filament", true));
            return on_main([=]() {
                if (f < 1 || f > int(bundle().filament_presets.size()))
                    bad_arg("filament slot out of range (1.." + std::to_string(bundle().filament_presets.size()) + ")");
                const int idx = object_index(id);
                select_objects_ui({idx});
                wxGetApp().obj_list()->set_extruder_for_selected_items(f);
                return object_json(size_t(idx));
            });
        });

    add("rename_object", "Rename object", "Rename an object.", schema({{"id", kObjectProp}, {"name", prop("string", "New name.")}}, {"id", "name"}), false,
        [](ToolContext &, const json &a) {
            const uint64_t    id   = uint64_t(int_arg(a, "id", true));
            const std::string name = str_arg(a, "name", true);
            return on_main([=]() {
                Plater   &p   = plater();
                const int idx = object_index(id);
                p.take_snapshot("Rename object");
                p.model().objects[idx]->name = name;
                wxGetApp().obj_list()->sync_name_from_model(idx, -1);
                p.set_plater_dirty(true);
                return object_json(size_t(idx));
            });
        });

    add("clone_object", "Clone object",
        "Make copies of an object (like Clone in the object's context menu). Each copy is a new object placed next to the original.",
        schema({{"id", kObjectProp}, {"copies", prop("integer", "Number of copies to add (1..100).")}}, {"id", "copies"}), false,
        [](ToolContext &, const json &a) {
            const uint64_t  id     = uint64_t(int_arg(a, "id", true));
            const long long copies = int_arg(a, "copies", true);
            if (copies < 1 || copies > 100) bad_arg("'copies' must be 1..100");
            return on_main([=]() {
                Plater   &p   = plater();
                const int idx = object_index(id);
                if (!p.is_view3D_shown()) wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                std::vector<uint64_t> before;
                for (auto *o : p.model().objects) before.push_back(o->id().id);
                select_objects_ui({idx});
                p.canvas3D()->get_selection().clone(int(copies));
                json added = json::array();
                for (size_t i = 0; i < p.model().objects.size(); ++i)
                    if (std::find(before.begin(), before.end(), p.model().objects[i]->id().id) == before.end()) added.push_back(object_json(i));
                return json{{"added", added}, {"objects", p.model().objects.size()}};
            });
        });

    add("arrange_objects", "Arrange objects", "Auto-arrange the objects (like the Arrange toolbar button) and wait for it. Like in the app, objects that do not fit spill onto further plates and empty plates are removed.", schema(), false,
        [](ToolContext &, const json &) {
            on_main([]() {
                Plater &p = plater();
                if (p.model().objects.empty()) throw ToolError{"arrange.empty", "there are no objects"};
                if (!p.is_view3D_shown()) wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                p.arrange();
                return json();
            });
            sleep_ms(200);
            wait_until([]() { return plater().is_any_job_running() ? json() : json(true); }, 300);
            return on_main([]() { return json{{"objects", objects_json()}, {"plates", plates_json()}}; });
        });

    add("orient_objects", "Auto-orient objects", "Auto-orient objects for printing (the given ids, or all), and wait.",
        schema({{"ids", arr_of("integer", "Object ids (default: all).")}}), false, [](ToolContext &, const json &a) {
            auto ids = int_list_arg(a, "ids");
            on_main([ids]() {
                Plater &p = plater();
                if (p.model().objects.empty()) throw ToolError{"orient.empty", "there are no objects"};
                if (!p.is_view3D_shown()) wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                if (ids.empty()) p.select_all();
                else {
                    std::vector<int> idx;
                    for (auto id : ids) idx.push_back(object_index(uint64_t(id)));
                    select_objects_ui(idx);
                }
                p.orient();
                return json();
            });
            sleep_ms(200);
            wait_until([]() { return plater().is_any_job_running() ? json() : json(true); }, 300);
            return on_main([]() { return json{{"objects", objects_json()}}; });
        });

    // ---------------- plates ----------------
    add("list_plates", "List plates", "All plates: number, name, bed type, print sequence, objects on it, and slice summary.", schema(), true,
        [](ToolContext &, const json &) {
            return on_main([]() { return json{{"plates", plates_json()}, {"current_plate", plater().get_partplate_list().get_curr_plate_index() + 1}}; });
        });

    add("add_plate", "Add plate", "Add an empty plate (like the + plate button). The new plate becomes current.", schema(), false,
        [](ToolContext &, const json &) {
            on_main([]() {
                Plater &p = plater();
                if (!p.can_add_plate()) throw ToolError{"plate.limit", "no more plates can be added"};
                if (!p.is_view3D_shown()) wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                wxPostEvent(p.canvas3D()->get_wxglcanvas(), SimpleEvent(EVT_GLTOOLBAR_ADD_PLATE));
                return json();
            });
            sleep_ms(300);
            return on_main([]() { return json{{"plates", plates_json()}, {"current_plate", plater().get_partplate_list().get_curr_plate_index() + 1}}; });
        });

    add("delete_plate", "Delete plate", "Delete a plate and the objects on it.", schema({{"plate", kPlateProp}}, {"plate"}), false,
        [](ToolContext &, const json &a) {
            return on_main([a]() {
                Plater   &p   = plater();
                const int idx = plate_index_arg(a, "plate", true);
                if (p.get_partplate_list().get_plate_count() <= 1) throw ToolError{"plate.last", "the last plate cannot be deleted"};
                p.take_snapshot("Delete plate");
                p.delete_plate(idx);
                return json{{"plates", plates_json()}, {"objects", objects_json()}};
            });
        });

    add("select_plate", "Select plate", "Make a plate the current one (like clicking it).", schema({{"plate", kPlateProp}}, {"plate"}), false,
        [](ToolContext &, const json &a) {
            return on_main([a]() {
                Plater   &p   = plater();
                const int idx = plate_index_arg(a, "plate", true);
                p.select_plate(idx);
                p.canvas3D()->select_plate();
                return plate_json(idx);
            });
        });

    add("set_plate_settings", "Set plate settings", "Set a plate's name, bed type ('default' = use the global bed type) or print sequence.",
        schema({{"plate", kPlateProp},
                {"name", prop("string", "Plate name.")},
                {"bed_type", prop("string", "Bed type name (see get_selected_presets.available_bed_types) or 'default'.")},
                {"print_sequence", enum_prop({"default", "by layer", "by object"}, "Print sequence.")}}),
        false, [](ToolContext &, const json &a) {
            auto name = opt_str(a, "name"), bed = opt_str(a, "bed_type"), seq = opt_str(a, "print_sequence");
            return on_main([=]() {
                Plater   &p     = plater();
                int       idx   = plate_index_arg(a, "plate");
                if (idx < 0) idx = p.get_partplate_list().get_curr_plate_index();
                PartPlate *pp = p.get_partplate_list().get_plate(idx);
                p.take_snapshot("Plate settings");
                if (name) pp->set_plate_name(*name);
                if (bed) {
                    if (*bed == "default") pp->set_bed_type(btDefault);
                    else {
                        const auto &vals = print_config_def.get("curr_bed_type")->enum_values;
                        auto        it   = std::find_if(vals.begin(), vals.end(), [&](const std::string &v) { return boost::iequals(v, *bed); });
                        if (it == vals.end()) bad_arg("unknown bed type '" + *bed + "'");
                        const auto &keys = print_config_def.get("curr_bed_type")->enum_keys_map;
                        auto        k    = keys->find(*it);
                        if (k == keys->end()) bad_arg("unknown bed type '" + *bed + "'");
                        pp->set_bed_type(BedType(k->second));
                    }
                }
                if (seq) {
                    if (*seq == "by layer") pp->set_print_seq(PrintSequence::ByLayer);
                    else if (*seq == "by object") pp->set_print_seq(PrintSequence::ByObject);
                    else if (*seq == "default") pp->set_print_seq(PrintSequence::ByDefault);
                    else bad_arg("'print_sequence' must be default, by layer or by object");
                }
                p.set_plater_dirty(true);
                p.update();
                p.get_current_canvas3D()->set_as_dirty();
                return plate_json(idx);
            });
        });

    add("move_object_to_plate", "Move object to plate", "Move an object onto another plate (centered on it; use arrange_objects afterwards to tidy).",
        schema({{"id", kObjectProp}, {"plate", kPlateProp}}, {"id", "plate"}), false, [](ToolContext &, const json &a) {
            const uint64_t id = uint64_t(int_arg(a, "id", true));
            return on_main([=]() {
                Plater     &p      = plater();
                const int   pidx   = plate_index_arg(a, "plate", true);
                const int   idx    = object_index(id);
                GLCanvas3D *canvas = p.canvas3D();
                if (!p.is_view3D_shown()) wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                PartPlate  *pp     = p.get_partplate_list().get_plate(pidx);
                const Vec3d target = pp->get_bounding_box().center();
                select_objects_ui({idx});
                const Vec3d c = p.model().objects[idx]->instance_bounding_box(0).center();
                Selection  &sel = canvas->get_selection();
                sel.setup_cache();
                sel.translate(Vec3d(target.x() - c.x(), target.y() - c.y(), 0.0), TransformationType(TransformationType::World_Relative_Joint));
                canvas->do_move("Move to plate");
                return object_json(size_t(object_index(id)));
            });
        });

    // ---------------- slicing / export ----------------
    add("slice", "Slice",
        "Slice the current plate (or 'plate', or all plates) like the Slice button, switch to the Preview and wait for the result "
        "(print time, filament, warnings). Sends progress notifications.",
        schema({{"plate", kPlateProp},
                {"all_plates", prop("boolean", "Slice all plates. Default false.")},
                {"wait", prop("boolean", "Wait for the result (default true).")},
                {"timeout_s", prop("number", "Max seconds to wait (default 900).")}}),
        false, [](ToolContext &ctx, const json &a) { return slice(ctx, a); });

    add("get_slice_status", "Get slice status", "Whether slicing is running, and the slice result of each plate.", schema(), true,
        [](ToolContext &, const json &) {
            return on_main([]() {
                Plater &p = plater();
                return json{{"slicing", p.is_background_process_slicing()},
                            {"error_plate", p.slice_error_plate() >= 0 ? json(p.slice_error_plate() + 1) : json()},
                            {"plates", plates_json()}};
            });
        });

    add("cancel_slice", "Cancel slicing", "Stop the running slice (like the Cancel button on the slicing notification).", schema(), false,
        [](ToolContext &, const json &) { return on_main([]() { return json{{"cancelled", plater().cancel_slicing()}}; }); });

    add("export_gcode", "Export G-code",
        "Write the sliced G-code of the current plate to 'path' (slice first). Waits until the file is written.",
        schema({{"path", prop("string", "Absolute output path (.gcode).")}, {"timeout_s", prop("number", "Max seconds to wait (default 300).")}}, {"path"}),
        false, [](ToolContext &ctx, const json &a) {
            const std::string path    = check_out_path(a, "path", ".gcode");
            const double      timeout = num_arg(a, "timeout_s", 300);
            struct W
            {
                std::mutex  m;
                bool        done = false;
                std::string err;
            };
            auto              w  = std::make_shared<W>();
            std::weak_ptr<W>  ww = w;
            auto sub = ctx.core.events().subscribe({core::Topic::Slice}, [ww, path](const core::Event &ev) {
                auto w = ww.lock();
                if (!w) return;
                std::lock_guard<std::mutex> lk(w->m);
                if (ev.key == core::slice_events::kExportFinished) {
                    if (auto *p = ev.payload_as<core::slice_events::ExportFinished>(); !p || p->path.empty() || fs::path(p->path) == fs::path(path))
                        w->done = true;
                } else if (ev.key == core::slice_events::kProcessCompleted) {
                    if (auto *p = ev.payload_as<core::slice_events::ProcessCompleted>(); p && p->status != core::slice_events::ProcessCompleted::Finished) {
                        w->err  = p->status == core::slice_events::ProcessCompleted::Cancelled ? "export was cancelled" : "export failed";
                        w->done = true;
                    }
                }
            });
            on_main([path]() {
                Plater    &p  = plater();
                PartPlate *pp = p.get_partplate_list().get_curr_plate();
                if (!pp->is_slice_result_valid()) throw ToolError{"export.not_sliced", "the current plate is not sliced; call slice first"};
                std::string err;
                if (!p.export_gcode_to(path, err)) throw ToolError{"export.failed", err.empty() ? "export failed" : err};
                return json();
            });
            const auto t0 = std::chrono::steady_clock::now();
            while (true) {
                {
                    std::lock_guard<std::mutex> lk(w->m);
                    if (w->done) break;
                }
                if (elapsed_s(t0) > timeout) throw ToolError{"export.timeout", "export did not finish in time"};
                sleep_ms(200);
            }
            if (!w->err.empty()) throw ToolError{"export.failed", w->err};
            sleep_ms(200);
            if (!fs::exists(path)) throw ToolError{"export.failed", "no file was written"};
            return json{{"path", path}, {"bytes", fs::file_size(path)}};
        });

    add("export_sliced_file", "Export sliced 3MF",
        "Write a .gcode.3mf (G-code plus thumbnails and slice info, the file the printer takes) of the current plate or all plates.",
        schema({{"path", prop("string", "Absolute output path (.gcode.3mf).")}, {"all_plates", prop("boolean", "All sliced plates. Default false.")}}, {"path"}),
        false, [](ToolContext &, const json &a) {
            std::string path = str_arg(a, "path", true);
            if (!boost::iends_with(path, ".gcode.3mf")) path += ".gcode.3mf";
            json a2    = a;
            a2["path"] = path;
            path       = check_out_path(a2, "path", nullptr);
            const bool all = bool_arg(a, "all_plates", false);
            return on_main([=]() {
                Plater &p = plater();
                PartPlateList &pl = p.get_partplate_list();
                if (!all && !pl.get_curr_plate()->is_slice_result_valid()) throw ToolError{"export.not_sliced", "the current plate is not sliced; call slice first"};
                const int plate_idx = all ? PLATE_ALL_IDX : pl.get_curr_plate_index();
                const int rc = p.export_3mf(fs::path(path), SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel, plate_idx);
                if (rc < 0) throw ToolError{"export.failed", "failed to write " + path};
                return json{{"path", path}, {"bytes", fs::exists(path) ? fs::file_size(path) : 0}};
            }, 600);
        });

    // ---------------- printers (read-only + connect) ----------------
    add("list_printers", "List printers",
        "Printers discovered on the LAN (SSDP) plus the selected one: id (serial), name, IP, model, online/connected, busy.", schema(), true,
        [](ToolContext &, const json &) {
            return on_main([]() {
                DeviceManager *d   = devices();
                MachineObject *sel = d->get_selected_machine();
                json           arr = json::array();
                bool           has_sel = false;
                for (auto &[id, m] : d->get_local_machinelist())
                    if (m) {
                        arr.push_back(printer_brief(m, sel));
                        has_sel = has_sel || m == sel;
                    }
                if (sel && !has_sel) arr.push_back(printer_brief(sel, sel));
                return json{{"printers", arr}};
            });
        });

    add("get_printer_status", "Get printer status",
        "Live status of a printer (the selected one by default): print state and progress, temperatures, AMS trays, HMS errors, and the latest print_plate operation sent to it (last_print_operation, see get_print_operation). Read-only.",
        schema({{"printer", kPrinterProp}}), true, [](ToolContext &, const json &a) {
            return on_main([a]() {
                MachineObject *o = printer_arg(a);
                json           j = printer_status(o);
                std::shared_ptr<SendOp> last;
                {
                    std::lock_guard<std::mutex> lk(g_ops_mutex);
                    for (auto &[id, op] : g_ops)
                        if (op->dev_id == o->get_dev_id() && (!last || op->created > last->created)) last = op;
                }
                if (last) j["last_print_operation"] = op_status_json(*last);
                return j;
            });
        });

    add("connect_printer", "Connect printer",
        "Select a discovered LAN printer and connect to it (like choosing it on the Device tab). Pass the printer's access code the "
        "first time; it is stored by the app like a manual entry. Read-only otherwise: nothing is started or moved.",
        schema({{"printer", kPrinterProp},
                {"access_code", prop("string", "8-character LAN access code from the printer's screen (only needed once).")},
                {"wait_s", prop("number", "Seconds to wait for the connection (default 20).")}},
               {"printer"}),
        false, [](ToolContext &, const json &a) {
            auto         code = opt_str(a, "access_code");
            const double wait = num_arg(a, "wait_s", 20);
            const std::string dev_id = on_main([=]() {
                DeviceManager *d = devices();
                MachineObject *o = printer_arg(a);
                if (code) o->set_user_access_code(*code);
                if (!o->has_access_right()) throw ToolError{"printer.no_access_code", "the printer needs its access code ('access_code')"};
                d->set_selected_machine(o->get_dev_id());
                wxGetApp().mainframe->jump_to_monitor(o->get_dev_id());
                return json(o->get_dev_id());
            }).get<std::string>();
            json r = wait_until([dev_id]() {
                MachineObject *o = devices()->get_selected_machine();
                return o && o->get_dev_id() == dev_id && o->is_connected() ? json(true) : json();
            }, wait, 500);
            return on_main([dev_id, ok = !r.is_null()]() {
                json j = printer_brief(devices()->get_selected_machine(), devices()->get_selected_machine());
                j["connected_within_wait"] = ok;
                return j;
            });
        });

    add("disconnect_printer", "Disconnect printer", "Disconnect from the selected printer (LAN MQTT). Nothing on the printer changes.", schema(), false,
        [](ToolContext &, const json &) {
            return on_main([]() {
                MachineObject *o = devices()->get_selected_machine();
                if (!o) throw ToolError{"printer.none_selected", "no printer is selected"};
                o->disconnect();
                return json{{"disconnected", o->get_dev_id()}};
            });
        });

    add("printer_camera_snapshot", "Printer camera snapshot",
        "Save one frame from a printer's camera as a PNG (needs ffmpeg on the computer; LAN RTSPS stream).",
        schema({{"printer", kPrinterProp}, {"path", prop("string", "Absolute output path (.png).")},
                {"timeout_s", prop("number", "Give up after this many seconds (default 20, 3..60).")}}, {"path"}), true, [](ToolContext &, const json &a) {
            const std::string path = check_out_path(a, "path", ".png");
            const double      timeout = std::clamp(num_arg(a, "timeout_s", 20), 3.0, 60.0);
            const std::string ffmpeg  = printbot::mcp::find_executable("ffmpeg");
            if (ffmpeg.empty()) throw ToolError{"camera.unavailable", "ffmpeg is not installed on this computer; the camera snapshot needs it"};
            const std::string url = on_main([a]() {
                MachineObject *o = printer_arg(a, true);
                if (!o->local_rtsp_url.empty() && o->local_rtsp_url != "disable") {
                    std::string u = o->local_rtsp_url;
                    auto        p = u.find("://");
                    if (p != std::string::npos && u.find('@') == std::string::npos) u.insert(p + 3, "bblp:" + o->get_access_code() + "@");
                    return json(u);
                }
                throw ToolError{"camera.unavailable", "the printer reports no LAN camera stream (enable LAN liveview on the printer)"};
            }).get<std::string>();
            // No shell: ffmpeg gets an argument vector, so nothing in the path or URL is interpreted.
            // Never log the URL or the arguments: the URL carries the access code.
            boost::system::error_code ec;
            fs::remove(path, ec);
            const auto r = printbot::mcp::run_process(printbot::mcp::ffmpeg_snapshot_argv(ffmpeg, url, path), timeout);
            if (!r.started) throw ToolError{"camera.failed", "could not start ffmpeg"};
            if (r.timed_out) throw ToolError{"camera.timeout", "no frame from the camera within " + std::to_string(int(timeout)) + " s"};
            if (r.exit_code != 0 || !fs::exists(path, ec) || fs::file_size(path, ec) == 0)
                throw ToolError{"camera.failed", "could not read a frame from the camera"};
            return json{{"path", path}, {"bytes", fs::file_size(path, ec)}};
        });

    // ---------------- print jobs (policy-gated) ----------------
    add("print_plate", "Print plate",
        "Send one sliced plate for one explicit human print request, without an approval popup. Default dry_run=true returns "
        "readiness, job summary and job_fingerprint. A real send requires printer control, dry_run=false, fingerprint, "
        "user_authorized=true and a stable authorization_id identifying that human request. Reuse the same ID for retries; "
        "it is persisted and consumed before the first send attempt, including failures. Never generate another ID unless "
        "the human issues another print instruction. Changed job facts are rejected. Poll get_print_operation for the result. " +
        printbot::mcp::print_approval_instructions(),
        schema({{"dry_run", prop("boolean", "Only check readiness and return the summary and job_fingerprint (default true).")},
                {"fingerprint", prop("string", "job_fingerprint from the current dry run. Required with dry_run=false.")},
                {"user_authorized", prop("boolean", "True only for an explicit human request to print this job; required for a real send.")},
                {"authorization_id", prop("string", "Stable unique ID for that human request, 1..128 ASCII letters, digits, hyphens or underscores. Reuse across retries and restarts; never derive from the job fingerprint.")},
                {"open_dialog", prop("boolean", "Open the Print dialog without sending (default false).")}}),
        false, [](ToolContext &, const json &a) {
            const bool        dry = bool_arg(a, "dry_run", true), open = bool_arg(a, "open_dialog", false);
            const std::string want_fp = str_arg(a, "fingerprint");
            const std::string authorization_id = str_arg(a, "authorization_id");
            if (!dry) {
                require_policy("starting a print");
                if (!bool_arg(a, "user_authorized", false))
                    throw ToolError{"print.not_authorized", "an explicit human print request is required; set user_authorized only for that request"};
                if (authorization_id.empty() || authorization_id.size() > 128 ||
                    authorization_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
                    throw ToolError{"invalid_argument", "authorization_id must contain 1..128 ASCII letters, digits, hyphens or underscores"};
                if (want_fp.empty())
                    throw ToolError{"print.fingerprint_required",
                                    "run print_plate with dry_run=true, then pass its job_fingerprint for the explicitly requested print"};
            }
            json pre = on_main([]() {
                Plater    &p  = plater();
                PartPlate *pp = p.get_partplate_list().get_curr_plate();
                json       j;
                j["plate"]   = p.get_partplate_list().get_curr_plate_index() + 1;
                j["sliced"]  = pp->is_slice_result_valid();
                j["control_allowed"] = printer_control_allowed();
                j["approval_required_before_every_print"] = true;
                j["approval"] = "one explicit human chat request authorizes one print attempt; no approval popup";
                j["approval_instructions"] = printbot::mcp::print_approval_instructions();
                std::map<uint64_t, json> by_object;
                for (const auto &oi : pp->get_obj_and_inst_set()) {
                    if (oi.first < 0 || oi.first >= int(p.model().objects.size())) continue;
                    const ModelObject *object = p.model().objects[oi.first];
                    if (oi.second < 0 || oi.second >= int(object->instances.size()) || !object->instances[oi.second]->printable) continue;
                    json &o = by_object[object->id().id];
                    if (o.is_null()) o = {{"name", object->name}, {"object_id", object->id().id}, {"instances", json::array()}, {"copies", 0}};
                    o["instances"].push_back(oi.second);
                    o["copies"] = o["copies"].get<int>() + 1;
                }
                json objects = json::array();
                for (auto &[id, o] : by_object) objects.push_back(o);
                j["job"] = {{"project", u8(p.get_project_name())}, {"path", p.get_3mf_filename()},
                            {"plate_name", pp->get_plate_name()}, {"objects", objects}, {"slice", slice_summary(pp)}};
                j["presets"] = selected_presets_json();
                json issues  = json::array();
                if (!pp->is_slice_result_valid()) issues.push_back("the current plate is not sliced");
                DeviceManager *d = wxGetApp().getDeviceManager();
                MachineObject *o = d ? d->get_selected_machine() : nullptr;
                if (!o) issues.push_back("no printer selected (connect_printer)");
                else {
                    j["printer"] = printer_brief(o, o);
                    if (!o->is_connected()) issues.push_back("printer not connected");
                    if (o->is_in_printing()) issues.push_back("printer is busy printing");
                }
                j["issues"]          = issues;
                j["ready"]           = issues.empty();
                j["job_fingerprint"] = sha256_hex(job_facts().dump());
                if (test_no_send()) j["test_mode"] = "PRINTBOT_MCP_TEST_NO_SEND=1: the gate never presses Send";
                return j;
            });
            if (dry && !open) {
                pre["dry_run"] = true;
                pre["would"]   = "open the Print dialog for plate " + std::to_string(pre["plate"].get<int>()) +
                               ", consume one explicit human print request, and send once without an approval popup";
                return pre;
            }
            if (!pre["sliced"].get<bool>()) throw ToolError{"print.not_sliced", "slice the plate first"};
            if (!dry) {
                if (!pre["ready"].get<bool>() && !test_no_send()) throw ToolError{"print.not_ready", pre["issues"].dump()};
                if (pre["job_fingerprint"].get<std::string>() != want_fp)
                    throw ToolError{"approval.stale", "the job changed since the summary with fingerprint " + want_fp.substr(0, 16) +
                                                          " (now " + pre["job_fingerprint"].get<std::string>().substr(0, 16) +
                                                          "). Run dry_run again and verify the job remains within the human request. Nothing was sent."};
            }
            const bool already_open = on_main([]() { return json(shown_select_machine_dialog() != nullptr); }).get<bool>();
            if (!already_open) post_to_main([]() { wxPostEvent(&plater(), SimpleEvent(EVT_GLTOOLBAR_PRINT_PLATE)); });
            json dlg = wait_until([]() {
                for (auto *w : shown_dialogs())
                    if (is_print_dialog(w)) return json(true);
                return json();
            }, 20);
            if (dlg.is_null()) throw ToolError{"print.no_dialog", "the Print dialog did not open"};
            if (dry) return json{{"dialog_open", true}, {"status", "not_sent"}, {"preflight", pre}};
            sleep_ms(1500); // the dialog fetches printer state / thumbnails / mapping
            return gated_send(pre, authorization_id);
        });

    add("get_print_operation", "Get print operation",
        "Outcome of a print_plate send, by operation_id (default: the latest). status is one of: send_requested, "
        "awaiting_user_confirmation (the Print dialog asks the user something), uploading, transferred_awaiting_printer, printing, "
        "paused, finished, printer_failed, failed (transfer failed or was cancelled), not_started (no transfer began; read the "
        "Print dialog's message), test_no_send, unknown. printing/paused/finished require the printer to report a matching new "
        "job. Unknown means unknown: inspect the printer and ask the user; never resend automatically. Read-only.",
        schema({{"operation_id", prop("string", "operation_id from print_plate (default: the latest).")}}), true, [](ToolContext &, const json &a) {
            const std::string id = str_arg(a, "operation_id");
            auto              op = find_op(id);
            if (!op) throw ToolError{"print.no_operation", id.empty() ? "no print operation yet" : "no print operation '" + id + "'"};
            return on_main([op]() { return op_status_json(*op); });
        });

    add("printer_control", "Printer control",
        "Pause, resume or stop the selected printer's current print. Default dry_run = true (reports what would be sent). A real "
        "command needs dry_run = false, confirm = true and printer control enabled by the user.",
        schema({{"printer", kPrinterProp},
                {"action", enum_prop({"pause", "resume", "stop"}, "Command.")},
                {"dry_run", prop("boolean", "Default true.")},
                {"confirm", prop("boolean", "Required with dry_run = false.")}},
               {"action"}),
        false, [](ToolContext &, const json &a) {
            const std::string action = str_arg(a, "action", true);
            if (action != "pause" && action != "resume" && action != "stop") bad_arg("'action' must be pause, resume or stop");
            const bool dry = bool_arg(a, "dry_run", true), confirm = bool_arg(a, "confirm", false);
            if (!dry) {
                require_policy("controlling a printer");
                if (!confirm) throw ToolError{"printer.not_confirmed", "pass confirm = true"};
            }
            return on_main([=]() {
                MachineObject *o = printer_arg(a, true);
                json           j = {{"printer", o->get_dev_id()}, {"action", action}, {"dry_run", dry}};
                const bool     ok = action == "pause" ? o->can_pause() : action == "resume" ? o->can_resume() : o->is_in_printing();
                j["possible"]    = ok;
                if (dry) return j;
                if (!ok) throw ToolError{"printer.not_possible", "'" + action + "' is not possible in the printer's current state"};
                const int rc = action == "pause" ? o->command_task_pause() : action == "resume" ? o->command_task_resume() : o->command_task_abort();
                j["sent"]    = rc == 0;
                return j;
            });
        });

    // ---------------- UI ----------------
    add("ui_get_state", "Get UI state", "Current tab, 3D/preview view, selected objects and open dialogs.", schema(), true,
        [](ToolContext &, const json &) { return on_main([]() { return ui_state_json(); }); });

    add("ui_switch_tab", "Switch tab", "Show a main tab: home, prepare (3D editor), preview or device (printer monitor).",
        schema({{"tab", enum_prop({"home", "prepare", "preview", "device"}, "Tab.")}}, {"tab"}), false, [](ToolContext &, const json &a) {
            const std::string t = str_arg(a, "tab", true);
            int               i = t == "home" ? MainFrame::tpHome : t == "prepare" ? MainFrame::tp3DEditor : t == "preview" ? MainFrame::tpPreview
                                  : t == "device" ? MainFrame::tpMonitor : -1;
            if (i < 0) bad_arg("'tab' must be home, prepare, preview or device");
            return on_main([i]() {
                wxGetApp().mainframe->select_tab(size_t(i));
                return ui_state_json();
            });
        });

    add("ui_set_camera", "Set camera", "Point the 3D/preview camera: a standard view and/or zoom to the bed or the objects.",
        schema({{"view", enum_prop({"iso", "top", "bottom", "front", "rear", "left", "right"}, "Standard view.")},
                {"zoom", enum_prop({"bed", "objects"}, "Zoom to fit.")}}),
        false, [](ToolContext &, const json &a) {
            auto view = opt_str(a, "view"), zoom = opt_str(a, "zoom");
            return on_main([=]() {
                Plater     &p = plater();
                GLCanvas3D *c = p.get_current_canvas3D();
                if (!c) throw ToolError{"ui.no_canvas", "no 3D view is shown"};
                if (view) p.select_view(*view);
                if (zoom && *zoom == "bed") c->zoom_to_bed();
                else if (zoom && *zoom == "objects") c->zoom_to_volumes();
                else if (zoom) bad_arg("'zoom' must be bed or objects");
                c->set_as_dirty();
                return ui_state_json();
            });
        });

    add("ui_screenshot", "Screenshot",
        "Save a PNG of the 3D/preview canvas (default), the whole window, or a dialog, so you can check what the app shows.",
        schema({{"path", prop("string", "Absolute output path (.png).")},
                {"target", enum_prop({"canvas", "window", "dialog"}, "What to capture (default canvas).")},
                {"dialog", json{{"description", "For target=dialog: index or title from ui_list_dialogs (default: the topmost)."}}}},
               {"path"}),
        true, [](ToolContext &, const json &a) {
            const std::string path   = check_out_path(a, "path", ".png");
            const std::string target = str_arg(a, "target", false).empty() ? "canvas" : str_arg(a, "target", false);
            if (target == "canvas") {
                struct Shot
                {
                    std::mutex                 m;
                    std::condition_variable    cv;
                    bool                       done = false;
                    int                        w = 0, h = 0;
                    std::vector<unsigned char> px;
                };
                auto shot = std::make_shared<Shot>();
                on_main([shot]() {
                    GLCanvas3D *c = plater().get_current_canvas3D();
                    if (!c) throw ToolError{"ui.no_canvas", "no 3D view is shown"};
                    c->request_frame_capture([shot](int w, int h, std::vector<unsigned char> &&px) {
                        std::lock_guard<std::mutex> lk(shot->m);
                        shot->w = w, shot->h = h, shot->px = std::move(px), shot->done = true;
                        shot->cv.notify_all();
                    });
                    c->set_as_dirty();
                    if (wxWindow *cw = c->get_wxglcanvas()) cw->Refresh();
                    return json();
                });
                std::unique_lock<std::mutex> lk(shot->m);
                if (!shot->cv.wait_for(lk, std::chrono::seconds(15), [&] { return shot->done; }))
                    throw ToolError{"ui.screenshot_timeout", "the canvas did not render a frame (is the window visible?)"};
                std::string err;
                if (!write_png(path, shot->w, shot->h, shot->px, err)) throw ToolError{"ui.screenshot_failed", err};
                return json{{"path", path}, {"width", shot->w}, {"height", shot->h}, {"target", "canvas"}};
            }
            if (target != "window" && target != "dialog") bad_arg("'target' must be canvas, window or dialog");
            return on_main([=]() -> json {
                wxWindow *w = wxGetApp().mainframe;
                if (target == "dialog") w = find_dialog(a);
#ifdef __WXGTK__
                GtkWidget *gw  = static_cast<GtkWidget *>(w->GetHandle());
                GdkWindow *win = gw ? gtk_widget_get_window(gw) : nullptr;
                if (!win) throw ToolError{"ui.screenshot_failed", "window is not realized"};
                // Read the screen area under the window (not the window itself) so dialogs and popups on top of it
                // are included, exactly as the user sees it.
                GdkWindow *top = gdk_window_get_toplevel(win);
                int        x = 0, y = 0;
                gdk_window_get_origin(top, &x, &y);
                GdkWindow *root = gdk_get_default_root_window();
                int        ww = gdk_window_get_width(top), hh = gdk_window_get_height(top);
                const int  rw = gdk_window_get_width(root), rh = gdk_window_get_height(root);
                if (x < 0) ww += x, x = 0;
                if (y < 0) hh += y, y = 0;
                ww = std::min(ww, rw - x), hh = std::min(hh, rh - y);
                if (ww <= 0 || hh <= 0) throw ToolError{"ui.screenshot_failed", "the window is off screen"};
                GdkPixbuf *pb = gdk_pixbuf_get_from_window(root, x, y, ww, hh);
                if (!pb) throw ToolError{"ui.screenshot_failed", "could not read the window (only supported on X11)"};
                GError *gerr = nullptr;
                const bool ok = gdk_pixbuf_save(pb, path.c_str(), "png", &gerr, nullptr);
                g_object_unref(pb);
                if (!ok) {
                    std::string m = gerr ? gerr->message : "save failed";
                    if (gerr) g_error_free(gerr);
                    throw ToolError{"ui.screenshot_failed", m};
                }
                return json{{"path", path}, {"width", ww}, {"height", hh}, {"target", target}};
#elif defined(__WXMSW__)
                if (!w) throw ToolError{"ui.screenshot_failed", "no window to capture"};
                // Copy the screen area under the window, so dialogs and popups on top of it are included.
                const wxRect r = w->GetScreenRect();
                if (r.width <= 0 || r.height <= 0) throw ToolError{"ui.screenshot_failed", "the window has no size"};
                wxBitmap bmp(r.width, r.height, 24);
                {
                    wxScreenDC sdc;
                    wxMemoryDC mdc(bmp);
                    mdc.Blit(0, 0, r.width, r.height, &sdc, r.x, r.y);
                }
                if (!bmp.ConvertToImage().SaveFile(wx(path), wxBITMAP_TYPE_PNG)) throw ToolError{"ui.screenshot_failed", "could not write " + path};
                return json{{"path", path}, {"width", r.width}, {"height", r.height}, {"target", target}};
#else
                throw ToolError{"ui.unsupported", "window and dialog screenshots are only implemented on Linux and Windows; use target=canvas"};
#endif
            });
        });

    add("ui_list_dialogs", "List dialogs", "Open dialogs and windows (index, title, class, modal).", schema(), true, [](ToolContext &, const json &) {
        return on_main([]() {
            json arr  = json::array();
            auto dlgs = shown_dialogs();
            for (size_t i = 0; i < dlgs.size(); ++i) arr.push_back(dialog_json(dlgs[i], i));
            return json{{"dialogs", arr}};
        });
    });

    const json kDialogProp = json{{"description", "Dialog index or (part of the) title from ui_list_dialogs. Default: the topmost."}};
    add("ui_dialog_inspect", "Inspect dialog", "List a dialog's text, fields, checkboxes, choices and buttons, numbered as 'widget'. For the Print dialog, also return the actual destination printer and filament mapping for the approval summary.",
        schema({{"dialog", kDialogProp}}), true, [](ToolContext &, const json &a) {
            return on_main([a]() {
                wxTopLevelWindow *d = find_dialog(a);
                json              w = json::array();
                collect_widgets(d, w, 0);
                json j       = dialog_json(d, 0);
                j.erase("index");
                j["widgets"] = w;
                if (auto *print = dynamic_cast<SelectMachineDialog *>(d)) {
                    if (MachineObject *machine = print->get_current_machine())
                        j["printer"] = printer_brief(machine, machine);
                    std::string legacy, mapping, info;
                    j["filament_mapping_valid"] = print->get_ams_mapping_result(legacy, mapping, info);
                    j["filament_mapping"] = mapping.empty() ? json(nullptr) : json::parse(mapping);
                    j["filament_mapping_info"] = info.empty() ? json(nullptr) : json::parse(info);
                }
                return j;
            });
        });

    add("ui_dialog_click", "Click dialog button",
        "Click a button in a dialog by label (e.g. 'OK', 'Don't save', 'Transfer') or widget number. Refused while a Print / Send "
        "dialog is open (prints go through print_plate with a single-use human chat authorization).",
        schema({{"dialog", kDialogProp}, {"button", prop("string", "Button label (case-insensitive).")}, {"widget", prop("integer", "Widget number.")}}),
        false, [](ToolContext &, const json &a) {
            auto      label  = opt_str(a, "button");
            const int widget = int(int_arg(a, "widget", false, -1));
            if (!label && widget < 0) bad_arg("give 'button' or 'widget'");
            // The click may open a nested modal loop; post it instead of running it inside the wait.
            on_main([=]() {
                refuse_in_print_dialogs();
                wxTopLevelWindow *d = find_dialog(a);
                wxWindow *b = label ? find_button(d, *label) : nth_widget(d, size_t(widget));
                if (!b) throw ToolError{"ui.no_widget", "no such button/widget in '" + u8(d->GetTitle()) + "'"};
                if (!b->IsEnabled()) throw ToolError{"ui.disabled", "'" + widget_label(b) + "' is disabled"};
                wxWindowID id = b->GetId();
                wxWeakRef<wxWindow> keep(b);
                post_to_main([keep, id]() {
                    if (!keep || is_print_window(keep.get()) || print_dialog_shown()) return; // re-checked at click time
                    if (auto *cb = dynamic_cast<wxCheckBox *>(keep.get())) {
                        cb->SetValue(!cb->GetValue());
                        wxCommandEvent e(wxEVT_CHECKBOX, id);
                        e.SetEventObject(cb);
                        e.SetInt(cb->GetValue());
                        cb->GetEventHandler()->ProcessEvent(e);
                    } else
                        click_window(keep.get());
                });
                return json();
            });
            sleep_ms(400);
            return on_main([]() { return ui_state_json(); });
        });

    add("ui_dialog_set", "Set dialog field", "Set a text field, checkbox, radio button or choice in a dialog (widget number from ui_dialog_inspect). Refused while a Print / Send dialog is open.",
        schema({{"dialog", kDialogProp}, {"widget", prop("integer", "Widget number.")}, {"value", json{{"description", "Text, boolean, or choice string."}}}},
               {"widget", "value"}),
        false, [](ToolContext &, const json &a) {
            const int widget = int(int_arg(a, "widget", true));
            if (!a.contains("value")) bad_arg("'value' is required");
            json v = a["value"];
            return on_main([=]() {
                refuse_in_print_dialogs();
                wxTopLevelWindow *d = find_dialog(a);
                wxWindow *c = nth_widget(d, size_t(widget));
                if (!c) throw ToolError{"ui.no_widget", "no widget " + std::to_string(widget)};
                const std::string s = v.is_string() ? v.get<std::string>() : v.dump();
                if (auto *t = dynamic_cast<wxTextCtrl *>(c)) t->SetValue(wx(s)); // SetValue emits wxEVT_TEXT
                else if (auto *cb = dynamic_cast<wxCheckBox *>(c)) {
                    cb->SetValue(v.is_boolean() ? v.get<bool>() : (s == "true" || s == "1"));
                    wxCommandEvent e(wxEVT_CHECKBOX, cb->GetId());
                    e.SetEventObject(cb);
                    e.SetInt(cb->GetValue());
                    cb->GetEventHandler()->ProcessEvent(e);
                } else if (auto *rb = dynamic_cast<wxRadioButton *>(c)) {
                    rb->SetValue(true);
                    wxCommandEvent e(wxEVT_RADIOBUTTON, rb->GetId());
                    e.SetEventObject(rb);
                    rb->GetEventHandler()->ProcessEvent(e);
                } else if (auto *co = dynamic_cast<wxComboBox *>(c)) {
                    int i = co->FindString(wx(s));
                    if (i == wxNOT_FOUND) {
                        co->SetValue(wx(s));
                    } else {
                        co->SetSelection(i);
                        wxCommandEvent e(wxEVT_COMBOBOX, co->GetId());
                        e.SetEventObject(co);
                        e.SetInt(i);
                        co->GetEventHandler()->ProcessEvent(e);
                    }
                } else if (auto *ch = dynamic_cast<wxChoice *>(c)) {
                    int i = ch->FindString(wx(s));
                    if (i == wxNOT_FOUND) throw ToolError{"ui.bad_value", "'" + s + "' is not an option"};
                    ch->SetSelection(i);
                    wxCommandEvent e(wxEVT_CHOICE, ch->GetId());
                    e.SetEventObject(ch);
                    e.SetInt(i);
                    ch->GetEventHandler()->ProcessEvent(e);
                } else
                    throw ToolError{"ui.not_editable", "widget " + std::to_string(widget) + " is not an editable field"};
                json w = json::array();
                collect_widgets(d, w, 0);
                return widget < int(w.size()) ? w[widget] : json();
            });
        });

    add("ui_close_dialog", "Close dialog", "Close a dialog like its window close button (Cancel).", schema({{"dialog", kDialogProp}}), false,
        [](ToolContext &, const json &a) {
            on_main([a]() {
                wxTopLevelWindow     *d = find_dialog(a);
                wxWeakRef<wxWindow> keep(d);
                post_to_main([keep]() {
                    auto *tlw = dynamic_cast<wxTopLevelWindow *>(keep.get());
                    if (!tlw) return;
                    auto *dlg = dynamic_cast<wxDialog *>(tlw);
                    if (dlg && dlg->IsModal()) dlg->EndModal(wxID_CANCEL);
                    else tlw->Close();
                });
                return json();
            });
            sleep_ms(300);
            return on_main([]() { return ui_state_json(); });
        });

    add("ui_open_dialog", "Open dialog",
        "Open an app dialog for the user or for inspection: preferences, printers (select printers), filaments (select filaments), "
        "printer_settings, process_settings, filament_settings, shortcuts, print (Print dialog, sends nothing), send (Send to printer "
        "storage, sends nothing), sync_info (the sidebar Sync Info action for nozzle/AMS information), sync_filaments (the "
        "Continue to sync filaments action). Complete filament synchronization before slicing and presenting each print approval. "
        "Inspect and handle any dialogs; opening the workflow does not prove synchronization completed. Sync can change presets and invalidate slices.",
        schema({{"dialog", enum_prop({"preferences", "printers", "filaments", "printer_settings", "process_settings", "filament_settings", "shortcuts", "print", "send", "sync_info", "sync_filaments"},
                                     "Dialog.")}},
               {"dialog"}),
        false, [](ToolContext &, const json &a) {
            const std::string d = str_arg(a, "dialog", true);
            on_main([d]() {
                Plater &p = plater();
                if (d == "sync_info" || d == "sync_filaments") {
                    printer_arg(json::object(), true);
                    if (!shown_dialogs().empty()) throw ToolError{"ui.blocked_by_dialog", "close or handle the current dialog before synchronizing printer information"};
                }
                if ((d == "print" || d == "send") && !p.get_partplate_list().get_curr_plate()->is_slice_result_valid())
                    throw ToolError{"print.not_sliced", "slice the plate first"};
                return json();
            });
            if (d == "sync_info") post_to_main([]() { wxGetApp().sidebar().deal_btn_sync(); });
            else if (d == "sync_filaments") post_to_main([]() { wxGetApp().sidebar().sync_ams_list(true); });
            else if (d == "preferences") post_to_main([]() { wxGetApp().open_preferences(); });
            else if (d == "printers") post_to_main([]() { PresetSelectionDialog::run(PresetSelectionDialog::pgPrinters); });
            else if (d == "filaments") post_to_main([]() { PresetSelectionDialog::run(PresetSelectionDialog::pgFilaments); });
            else if (d == "shortcuts") post_to_main([]() { wxGetApp().keyboard_shortcuts(); });
            else if (d == "print") post_to_main([]() { wxPostEvent(&plater(), SimpleEvent(EVT_GLTOOLBAR_PRINT_PLATE)); });
            else if (d == "send") post_to_main([]() { wxPostEvent(&plater(), SimpleEvent(EVT_GLTOOLBAR_SEND_TO_PRINTER)); });
            else if (d == "printer_settings" || d == "process_settings" || d == "filament_settings") {
                const Preset::Type t = d == "printer_settings" ? Preset::TYPE_PRINTER : d == "process_settings" ? Preset::TYPE_PRINT : Preset::TYPE_FILAMENT;
                post_to_main([t]() {
                    Tab *tab = wxGetApp().get_tab(t);
                    if (!tab) return;
                    if (tab->GetParent() == wxGetApp().params_panel()) wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
                    else wxGetApp().params_dialog()->Popup();
                    tab->restore_last_select_item();
                });
            } else
                bad_arg("unknown dialog '" + d + "'");
            sleep_ms(800);
            return on_main([]() { return ui_state_json(); });
        });

    return tools;
}

}}} // namespace Slic3r::GUI::pbmcp
