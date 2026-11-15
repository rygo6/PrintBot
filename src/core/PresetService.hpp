// print_bot core: presets (printer / process / filament selection) over libslic3r's
// PresetBundle. GUI-free (no wxWidgets).
#ifndef printbot_core_PresetService_hpp_
#define printbot_core_PresetService_hpp_

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Result.hpp"
#include "Snapshots.hpp"

namespace Slic3r {
class AppConfig;
class PresetBundle;
class DynamicPrintConfig;
} // namespace Slic3r

namespace printbot::core {

class Core;

class PresetService
{
public:
    enum class Kind { Printer, Process, Filament };

    struct PresetInfo
    {
        std::string name;
        std::string alias;
        std::string inherits;
        std::string vendor;
        std::string printer_model; // printers
        std::string nozzle;        // printers
        std::string filament_type; // filaments
        std::string filament_vendor;
        bool        system     = false;
        bool        user       = false;
        bool        compatible = true;
        bool        visible    = true;
        bool        selected   = false;
    };

    struct ListQuery
    {
        Kind        kind            = Kind::Printer;
        bool        compatible_only = true; // relative to the selected printer (and process)
        bool        visible_only    = false;
        std::string filter;                 // case-insensitive substring on name/alias
        size_t      limit           = 500;
    };

    struct ListResult
    {
        std::vector<PresetInfo> presets;
        size_t                  total_matching = 0;
    };

    struct Selection
    {
        std::optional<std::string>              printer;
        std::optional<std::string>              process;
        std::optional<std::vector<std::string>> filaments;
        std::optional<std::string>              bed_type;
        bool                                    allow_incompatible = false;
    };

    explicit PresetService(Core &core);
    ~PresetService();

    // --- thread-safe API (marshals onto the core thread) ---
    std::shared_ptr<const PresetSnapshot> snapshot() const;
    Result<ListResult>                    list(const ListQuery &q);
    Result<PresetSnapshot>                select(const Selection &sel);
    static std::vector<std::string>       bed_types();

    // --- core-thread only ---
    Status                     load(std::vector<std::string> &warnings);
    Slic3r::PresetBundle      &bundle() { return *m_bundle; }
    // Full config for slicing the current selection (single plate, default filament mapping).
    Slic3r::DynamicPrintConfig full_config() const;
    bool                       is_bbl_printer() const;

private:
    void        publish(const char *why);
    bool        set_bed_type(const std::string &name);
    void        apply_default_bed_type();
    void        apply_printer_defaults(bool model_changed);
    std::string resolve_name(Kind kind, const std::string &name) const;
    std::string suggestions(Kind kind, const std::string &name) const;

    Core                                  &m_core;
    std::unique_ptr<Slic3r::AppConfig>     m_app_config;
    std::unique_ptr<Slic3r::PresetBundle>  m_bundle;
    mutable std::mutex                     m_snap_mutex;
    std::shared_ptr<const PresetSnapshot>  m_snapshot;
    Revision                               m_rev = 0;
    std::string                            m_printer_model; // model the printer defaults were applied for
};

} // namespace printbot::core

#endif
