// print_bot core: immutable state snapshots. Rebuilt on the core thread after every
// mutation and published as shared_ptr<const T>; any thread may read them.
// Only essential state lives here: no tabs, views, camera or selection (those are UI state).
// GUI-free (no wxWidgets).
#ifndef printbot_core_Snapshots_hpp_
#define printbot_core_Snapshots_hpp_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Result.hpp"

namespace printbot::core {

using Revision = uint64_t;
using ObjectId = uint64_t;
using JobId    = uint64_t;

struct PresetSnapshot
{
    Revision                 rev = 0;
    std::string              printer;
    std::string              process;
    std::vector<std::string> filaments;
    std::string              bed_type;
    std::string              printer_model;
    std::vector<double>      nozzle_diameters;
    int                      extruder_count = 1;
    std::array<double, 2>    bed_min{0, 0};
    std::array<double, 2>    bed_max{0, 0};
    double                   max_print_height = 0;
};

struct ObjectInfo
{
    ObjectId              id = 0;
    std::string           name;
    std::string           source;
    int                   instances = 0;
    int                   volumes   = 0;
    size_t                facets    = 0;
    std::array<double, 3> min{0, 0, 0};
    std::array<double, 3> max{0, 0, 0};
    std::string           print_volume; // "inside", "partly_outside", "outside"
};

struct ProjectSnapshot
{
    Revision                rev = 0;
    std::vector<ObjectInfo> objects;
};

struct SliceWarning
{
    std::string level; // "info", "warning", "error"
    std::string code;
    std::string message;
};

struct FilamentUse
{
    int         index = 0; // 1-based filament slot
    std::string preset;
    std::string type;
    double      used_mm = 0;
    double      used_g  = 0;
};

struct SliceSummary
{
    double                    print_time_s   = 0;
    std::string               print_time;
    double                    prepare_time_s = 0;
    double                    filament_used_mm = 0;
    double                    filament_used_g  = 0;
    double                    filament_cost    = 0;
    std::vector<FilamentUse>  filaments;
    int                       layer_count = 0;
    double                    max_z       = 0;
    int                       objects     = 0;
    std::vector<SliceWarning> warnings;
    std::string               gcode_temp_path;
    uint64_t                  gcode_bytes = 0;
    double                    wall_time_s = 0;
};

struct SliceSnapshot
{
    enum class State { Idle, Running, Done, Error, Cancelled };
    State                       state = State::Idle;
    JobId                       job   = 0;
    int                         percent = 0;
    std::string                 message;
    Revision                    project_rev = 0; // inputs this result was sliced from
    Revision                    presets_rev = 0;
    bool                        outdated    = false; // inputs changed since
    std::optional<SliceSummary> summary;
    std::optional<Error>        error;
};

const char *to_string(SliceSnapshot::State s);

} // namespace printbot::core

#endif
