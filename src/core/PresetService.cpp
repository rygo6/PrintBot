#include "PresetService.hpp"

#include <algorithm>
#include <cctype>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "Core.hpp"
#include "Json.hpp"

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace printbot::core {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

const char *kind_name(PresetService::Kind k)
{
    switch (k) {
    case PresetService::Kind::Printer: return "printer";
    case PresetService::Kind::Process: return "process";
    case PresetService::Kind::Filament: return "filament";
    }
    return "?";
}

PresetCollection &collection(PresetBundle &b, PresetService::Kind k)
{
    switch (k) {
    case PresetService::Kind::Printer: return b.printers;
    case PresetService::Kind::Process: return b.prints;
    case PresetService::Kind::Filament: return b.filaments;
    }
    return b.printers;
}

// Copy the bundled vendor profiles into <data_dir>/system when they are missing,
// mirroring what the GUI does on first start. Never overwrites existing files.
void ensure_system_profiles(std::vector<std::string> &warnings)
{
    const fs::path src = fs::path(resources_dir()) / "profiles";
    const fs::path dst = fs::path(data_dir()) / PRESET_SYSTEM_DIR;
    if (!fs::exists(src)) {
        warnings.push_back("bundled profiles not found at " + src.string());
        return;
    }
    fs::create_directories(dst);
    for (const auto &entry : fs::directory_iterator(src)) {
        const fs::path p = entry.path();
        if (p.extension() != ".json" || !fs::is_directory(src / p.stem())) continue; // vendor bundle = X.json + X/
        if (fs::exists(dst / p.filename())) continue;
        // Copy the vendor folder first and the index last, so an interrupted copy is retried.
        size_t skipped = 0;
        for (fs::recursive_directory_iterator it(src / p.stem()), end; it != end; ++it) {
            const fs::path            rel = fs::relative(it->path(), src);
            boost::system::error_code ec;
            if (fs::is_directory(it->path(), ec)) fs::create_directories(dst / rel, ec);
            else if (!fs::exists(dst / rel, ec)) {
                fs::create_directories((dst / rel).parent_path(), ec);
                fs::copy_file(it->path(), dst / rel, ec);
            }
            if (ec) ++skipped; // e.g. dangling symlinks in the bundle
        }
        if (skipped) warnings.push_back(std::to_string(skipped) + " file(s) in profiles/" + p.stem().string() + " could not be copied (ignored)");
        fs::copy_file(p, dst / p.filename());
        BOOST_LOG_TRIVIAL(info) << "printbot core: installed system profiles " << p.stem().string() << " into " << dst.string();
    }
}

} // namespace

PresetService::PresetService(Core &core) : m_core(core) {}
PresetService::~PresetService() = default;

std::vector<std::string> PresetService::bed_types()
{
    const ConfigOptionDef *def = print_config_def.get("curr_bed_type");
    return def ? def->enum_values : std::vector<std::string>{};
}

Status PresetService::load(std::vector<std::string> &warnings)
{
    try {
        ensure_system_profiles(warnings);
        m_app_config = std::make_unique<AppConfig>();
        if (m_app_config->exists()) {
            // Read-only: the core never saves the GUI's app config.
            std::string err = m_app_config->load();
            if (!err.empty()) warnings.push_back("app config: " + err);
        }
        m_bundle = std::make_unique<PresetBundle>();
        m_bundle->setup_directories();
        auto [substitutions, errors] = m_bundle->load_presets(*m_app_config, ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        if (!errors.empty()) warnings.push_back("preset load: " + errors);
        if (!substitutions.empty()) warnings.push_back(std::to_string(substitutions.size()) + " preset(s) had unknown values substituted");
    } catch (const std::exception &ex) {
        return make_error("presets.load_failed", ex.what());
    }
    // Bed type: the user's last choice for this printer, else the printer model default.
    std::string user_bed;
    if (m_app_config->has_section("user_bed_type_list")) {
        auto sec = m_app_config->get_section("user_bed_type_list");
        auto it  = sec.find(m_bundle->printers.get_edited_preset().name);
        if (it != sec.end()) user_bed = it->second;
    }
    if (user_bed.empty() || !set_bed_type(user_bed)) apply_default_bed_type();
    if (!m_bundle->printers.get_selected_preset().is_default) apply_printer_defaults(true);
    publish("load");
    return Ok{};
}

bool PresetService::set_bed_type(const std::string &name)
{
    const auto types = bed_types();
    for (size_t i = 0; i < types.size(); ++i)
        if (lower(types[i]) == lower(name)) {
            m_bundle->project_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(BedType(i + 1)));
            return true;
        }
    return false;
}

// Printer-switch side effects that live in TabPrinter::on_preset_loaded in the GUI
// (nozzle volume types, nozzle stats, per-extruder AMS slots, purge mode). Ported here so a
// headless printer switch produces the same project config as the GUI.
void PresetService::apply_printer_defaults(bool model_changed)
{
    PresetBundle &b       = *m_bundle;
    const Preset &printer = b.printers.get_selected_preset();
    const Preset *base    = b.printers.get_preset_base(printer);
    if (!base) base = &printer;
    const std::string base_name = base->name;
    const int extruders = b.get_printer_extruder_count();
    if (int(b.extruder_ams_counts.size()) != extruders) b.extruder_ams_counts.resize(extruders);

    if (auto *nvt = b.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)) {
        std::string prev = m_app_config ? m_app_config->get_nozzle_volume_types_from_config(base_name) : std::string();
        if (prev.empty() || !nvt->deserialize(prev)) {
            if (auto *def = printer.config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type")) nvt->values = def->values;
        }
        nvt->values.resize(size_t(std::max(1, extruders)), int(nvtStandard));
        for (size_t eid = 0; eid < nvt->values.size(); ++eid) b.extruder_nozzle_stat.on_volume_type_switch(int(eid), NozzleVolumeType(nvt->values[eid]));
    }
    if (auto *o = b.project_config.option<ConfigOptionBool>("enable_filament_dynamic_map")) o->value = false;
    if (auto *o = b.project_config.option<ConfigOptionBool>("has_filament_switcher")) o->value = false;

    if (model_changed) {
        auto *max_nozzles = printer.config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
        auto *nvt         = b.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
        bool  multi_nozzle = false;
        if (max_nozzles && nvt) {
            multi_nozzle = std::any_of(max_nozzles->values.begin(), max_nozzles->values.end(), [](int i) { return i > 1; });
            b.extruder_nozzle_stat.on_printer_model_change(&b);
        }
        b.extruder_nozzle_stat.set_nozzle_data_flag(ExtruderNozzleStat::ndfNone);
        auto *fast_opt   = printer.config.option<ConfigOptionBool>("support_fast_purge_mode");
        bool  fast_purge = fast_opt ? fast_opt->value : false;
        if (auto *pvm = b.project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")) {
            if (!(multi_nozzle || fast_purge)) pvm->value = PrimeVolumeMode::pvmDefault;
            else if (!multi_nozzle && pvm->value == PrimeVolumeMode::pvmSaving) pvm->value = PrimeVolumeMode::pvmDefault;
            else if (!fast_purge && pvm->value == PrimeVolumeMode::pvmFast) pvm->value = PrimeVolumeMode::pvmDefault;
        }
    }
    m_printer_model = printer.config.has("printer_model") ? printer.config.opt_string("printer_model") : std::string();
}

void PresetService::apply_default_bed_type()
{
    const Preset &printer = m_bundle->printers.get_edited_preset();
    if (const VendorProfile::PrinterModel *pm = PresetUtils::system_printer_model(printer); pm && !pm->default_bed_type.empty())
        set_bed_type(pm->default_bed_type);
}

void PresetService::publish(const char *why)
{
    auto snap = std::make_shared<PresetSnapshot>();
    snap->rev = ++m_rev;
    const Preset &printer = m_bundle->printers.get_edited_preset();
    snap->printer   = m_bundle->printers.get_selected_preset_name();
    snap->process   = m_bundle->prints.get_selected_preset_name();
    snap->filaments = m_bundle->filament_presets;
    if (auto *bt = m_bundle->project_config.option<ConfigOptionEnum<BedType>>("curr_bed_type")) {
        const auto types = bed_types();
        int        idx   = int(bt->value) - 1;
        if (idx >= 0 && idx < int(types.size())) snap->bed_type = types[idx];
    }
    if (printer.config.has("printer_model")) snap->printer_model = printer.config.opt_string("printer_model");
    if (auto *nd = printer.config.option<ConfigOptionFloats>("nozzle_diameter")) snap->nozzle_diameters = nd->values;
    snap->extruder_count = m_bundle->get_printer_extruder_count();
    if (auto *area = printer.config.option<ConfigOptionPoints>("printable_area"); area && !area->values.empty()) {
        BoundingBoxf bb(area->values);
        snap->bed_min = {bb.min.x(), bb.min.y()};
        snap->bed_max = {bb.max.x(), bb.max.y()};
    }
    if (auto *h = printer.config.option<ConfigOptionFloat>("printable_height")) snap->max_print_height = h->value;
    {
        std::lock_guard<std::mutex> lk(m_snap_mutex);
        m_snapshot = snap;
    }
    nlohmann::json detail = to_json(*snap);
    detail["reason"]      = why;
    m_core.events().publish(Topic::Presets, "presets.changed", std::move(detail));
}

std::shared_ptr<const PresetSnapshot> PresetService::snapshot() const
{
    std::lock_guard<std::mutex> lk(m_snap_mutex);
    return m_snapshot;
}

std::string PresetService::resolve_name(Kind kind, const std::string &name) const
{
    PresetCollection &coll = collection(*m_bundle, kind);
    if (const Preset *p = coll.find_preset(name, false); p && !p->is_default) return p->name;
    const std::string &by_alias = coll.get_preset_name_by_alias(name);
    if (!by_alias.empty() && by_alias != name) {
        if (const Preset *p = coll.find_preset(by_alias, false); p && !p->is_default) return p->name;
    }
    return {};
}

std::string PresetService::suggestions(Kind kind, const std::string &name) const
{
    PresetCollection        &coll = collection(*m_bundle, kind);
    std::string              needle = lower(name);
    std::vector<std::string> hits;
    // Try progressively shorter prefixes of the first word for a useful hint.
    for (const Preset &p : coll)
        if (lower(p.name).find(needle) != std::string::npos && hits.size() < 8) hits.push_back(p.name);
    if (hits.empty()) {
        auto sp = needle.find(' ');
        std::string first = needle.substr(0, sp);
        for (const Preset &p : coll)
            if (!first.empty() && lower(p.name).find(first) != std::string::npos && p.is_compatible && hits.size() < 8) hits.push_back(p.name);
    }
    std::string out;
    for (const auto &h : hits) out += (out.empty() ? "" : "; ") + h;
    return out.empty() ? "" : " Did you mean: " + out;
}

Result<PresetService::ListResult> PresetService::list(const ListQuery &q)
{
    return m_core.executor().call([&]() -> Result<ListResult> {
        if (!m_bundle) return make_error("presets.not_loaded", "presets are not loaded");
        PresetCollection &coll     = collection(*m_bundle, q.kind);
        const std::string selected = q.kind == Kind::Filament ? std::string() : coll.get_selected_preset_name();
        const std::string needle   = lower(q.filter);
        ListResult        res;
        for (const Preset &p : coll) {
            if (p.is_default) continue;
            if (q.compatible_only && q.kind != Kind::Printer && !p.is_compatible) continue;
            if (q.visible_only && !p.is_visible) continue;
            if (!needle.empty() && lower(p.name).find(needle) == std::string::npos && lower(p.alias).find(needle) == std::string::npos) continue;
            ++res.total_matching;
            if (res.presets.size() >= q.limit) continue;
            PresetInfo info;
            info.name       = p.name;
            info.alias      = p.alias;
            info.inherits   = p.inherits();
            info.vendor     = p.vendor ? p.vendor->name : std::string();
            info.system     = p.is_system;
            info.user       = p.is_user();
            info.compatible = p.is_compatible;
            info.visible    = p.is_visible;
            if (q.kind == Kind::Filament)
                info.selected = std::find(m_bundle->filament_presets.begin(), m_bundle->filament_presets.end(), p.name) != m_bundle->filament_presets.end();
            else
                info.selected = p.name == selected;
            if (q.kind == Kind::Printer) {
                if (p.config.has("printer_model")) info.printer_model = p.config.opt_string("printer_model");
                if (auto *nd = p.config.option<ConfigOptionFloats>("nozzle_diameter"); nd && !nd->values.empty()) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%g", nd->values.front());
                    info.nozzle = buf;
                }
            } else if (q.kind == Kind::Filament) {
                if (auto *ft = p.config.option<ConfigOptionStrings>("filament_type"); ft && !ft->values.empty()) info.filament_type = ft->values.front();
                if (auto *fv = p.config.option<ConfigOptionStrings>("filament_vendor"); fv && !fv->values.empty()) info.filament_vendor = fv->values.front();
            }
            res.presets.push_back(std::move(info));
        }
        return res;
    });
}

namespace {
// Everything select() can change, so a failed selection can be undone completely.
// (PresetBundle's own copy does not cover the per-extruder state.)
struct BundleBackup
{
    PresetBundle                          bundle;
    std::vector<std::map<int, int>>       extruder_ams_counts;
    ExtruderNozzleStat                    extruder_nozzle_stat;
    std::map<int, DynamicPrintConfig>     filament_ams_list;
    std::vector<std::vector<std::string>> ams_multi_color_filment;
    std::string                           printer_model;

    BundleBackup(const PresetBundle &b, const std::string &model)
        : bundle(b), extruder_ams_counts(b.extruder_ams_counts), extruder_nozzle_stat(b.extruder_nozzle_stat),
          filament_ams_list(b.filament_ams_list), ams_multi_color_filment(b.ams_multi_color_filment), printer_model(model)
    {}
    void restore(PresetBundle &b, std::string &model) const
    {
        b                         = bundle;
        b.extruder_ams_counts     = extruder_ams_counts;
        b.extruder_nozzle_stat    = extruder_nozzle_stat;
        b.filament_ams_list       = filament_ams_list;
        b.ams_multi_color_filment = ams_multi_color_filment;
        model                     = printer_model;
    }
};

// What a published snapshot is made of; used to check that a rollback really restored it.
std::string selection_key(const PresetBundle &b)
{
    std::string k = b.printers.get_selected_preset_name() + "\n" + b.prints.get_selected_preset_name();
    for (const auto &f : b.filament_presets) k += "\n" + f;
    if (auto *bt = b.project_config.option<ConfigOptionEnum<BedType>>("curr_bed_type")) k += "\nbed " + std::to_string(int(bt->value));
    return k;
}
} // namespace

Result<PresetSnapshot> PresetService::select(const Selection &sel)
{
    return m_core.executor().call([&]() -> Result<PresetSnapshot> {
        if (!m_bundle) return make_error("presets.not_loaded", "presets are not loaded");
        // 1. Resolve and validate everything that does not depend on the new printer, so a bad
        //    name or bed type returns before anything changes.
        std::string printer;
        if (sel.printer) {
            printer = resolve_name(Kind::Printer, *sel.printer);
            if (printer.empty())
                return make_error("preset.not_found", "printer preset '" + *sel.printer + "' not found." + suggestions(Kind::Printer, *sel.printer));
        }
        if (sel.bed_type) {
            bool known = false;
            for (const auto &b : bed_types()) known |= lower(b) == lower(*sel.bed_type);
            if (!known) {
                std::string all;
                for (const auto &b : bed_types()) all += (all.empty() ? "" : ", ") + b;
                return make_error("bed_type.unknown", "unknown bed type '" + *sel.bed_type + "'. Known: " + all);
            }
        }
        std::string process;
        if (sel.process) {
            process = resolve_name(Kind::Process, *sel.process);
            if (process.empty())
                return make_error("preset.not_found", "process preset '" + *sel.process + "' not found." + suggestions(Kind::Process, *sel.process));
        }
        std::vector<std::string> filaments;
        if (sel.filaments) {
            if (sel.filaments->empty()) return make_error("preset.invalid", "filaments must name at least one filament preset");
            for (const auto &f : *sel.filaments) {
                std::string name = resolve_name(Kind::Filament, f);
                if (name.empty())
                    return make_error("preset.not_found", "filament preset '" + f + "' not found." + suggestions(Kind::Filament, f));
                filaments.push_back(name);
            }
        }

        // 2. Compatibility depends on the printer being selected, so apply the selection to the
        //    live bundle with a full backup and roll back on any failure: a failed call leaves
        //    presets, published status and slices exactly as they were.
        const std::string before_key = selection_key(*m_bundle);
        BundleBackup      backup(*m_bundle, m_printer_model);
        auto rollback = [&](Error err) -> Result<PresetSnapshot> {
            try {
                backup.restore(*m_bundle, m_printer_model);
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(error) << "printbot core: preset rollback failed: " << ex.what();
            }
            if (selection_key(*m_bundle) != before_key) {
                // Could not restore exactly: publish what is really selected (new revision, so
                // existing slices are reported outdated) rather than hide the change.
                publish("select_failed");
                err.message += " (the previous selection could not be fully restored; see get_status)";
            }
            return err;
        };
        try {
            if (!printer.empty() && printer != m_bundle->printers.get_selected_preset_name()) {
                m_bundle->printers.select_preset_by_name(printer, true, true);
                const Preset &sel_printer = m_bundle->printers.get_selected_preset();
                const std::string new_model = sel_printer.config.has("printer_model") ? sel_printer.config.opt_string("printer_model") : std::string();
                apply_printer_defaults(new_model != m_printer_model);
                // Re-pick process/filaments only where they became incompatible (prefers the
                // printer's default_print_profile / default_filament_profile), like the GUI.
                m_bundle->update_compatible(PresetSelectCompatibleType::Always);
                apply_default_bed_type();
            }

            if (!process.empty()) {
                const Preset *p = m_bundle->prints.find_preset(process, false);
                if (p && !p->is_compatible && !sel.allow_incompatible)
                    return rollback(make_error("preset.incompatible", "process preset '" + process + "' is not compatible with printer '" +
                                                                         m_bundle->printers.get_selected_preset_name() + "' (pass allow_incompatible to force)"));
                m_bundle->prints.select_preset_by_name(process, true, true);
                m_bundle->update_compatible(PresetSelectCompatibleType::Never);
            }

            if (!filaments.empty()) {
                for (const auto &name : filaments) {
                    const Preset *p = m_bundle->filaments.find_preset(name, false);
                    if (p && !p->is_compatible && !sel.allow_incompatible)
                        return rollback(make_error("preset.incompatible", "filament preset '" + name + "' is not compatible with printer '" +
                                                                             m_bundle->printers.get_selected_preset_name() + "' (pass allow_incompatible to force)"));
                }
                m_bundle->set_num_filaments(unsigned(filaments.size()));
                for (size_t i = 0; i < filaments.size(); ++i) m_bundle->filament_presets[i] = filaments[i];
                m_bundle->filaments.select_preset_by_name(filaments.front(), true, true);
                // Filament colours follow the presets' default colours.
                if (auto *colors = m_bundle->project_config.option<ConfigOptionStrings>("filament_colour")) {
                    colors->values.resize(filaments.size());
                    for (size_t i = 0; i < filaments.size(); ++i)
                        if (const Preset *p = m_bundle->filaments.find_preset(filaments[i], false))
                            if (auto *c = p->config.option<ConfigOptionStrings>("filament_colour"); c && !c->values.empty()) colors->values[i] = c->values.front();
                }
                m_bundle->update_compatible(PresetSelectCompatibleType::Never);
            }

            if (sel.bed_type) set_bed_type(*sel.bed_type);
        } catch (const std::exception &ex) {
            return rollback(make_error("preset.select_failed", ex.what()));
        }
        publish("select");
        return *snapshot();
    });
}

DynamicPrintConfig PresetService::full_config() const
{
    const int extruders = m_bundle->get_printer_extruder_count();
    if (extruders > 1) {
        // Default mapping: every filament on extruder 1 (phase 1; per-plate maps come with the project service).
        std::vector<int> f_maps(m_bundle->filament_presets.size(), 1);
        std::vector<int> f_volume_maps = m_bundle->get_default_nozzle_volume_types_for_filaments(f_maps);
        return m_bundle->full_config(false, f_maps, f_volume_maps);
    }
    return m_bundle->full_config(false);
}

bool PresetService::is_bbl_printer() const
{
    return m_bundle->printers.get_edited_preset().is_bbl_vendor_preset(m_bundle.get());
}

} // namespace printbot::core
