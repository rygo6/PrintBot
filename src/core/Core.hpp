// print_bot core: the GUI-free application core. One Core owns the essential state
// (presets, project/model, slicing) behind services, an event bus and a single core
// executor ("core thread"). Front ends (the MCP server, the UI) send commands to the
// services and redraw from snapshots/events. View state (tabs, Prepare vs Preview,
// camera, selection, dialogs) is NOT core state.
//
// Hard rule: nothing under src/core may include wxWidgets or slic3r/GUI headers
// (enforced at build time by check_no_gui.cmake).
#ifndef printbot_core_Core_hpp_
#define printbot_core_Core_hpp_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "EventBus.hpp"
#include "Executor.hpp"
#include "Result.hpp"
#include "Snapshots.hpp"

namespace printbot::core {

class PresetService;
class ProjectService;
class SliceService;

struct CoreOptions
{
    enum class Mode {
        // Core owns presets/model/slicing and runs its own core thread (MCP server, tests).
        Headless,
        // Hosted inside the wx GUI, which still owns presets/model (phase 1 interim):
        // only the event bus and an executor bound to the host's main loop.
        Embedded,
    };
    Mode mode = Mode::Headless;

    // Headless: resources (profiles, shapes) and the user data dir (system/ + user/ presets,
    // <app>.conf with the last selections). The data dir is only read, except that the
    // bundled system profiles are copied into <data_dir>/system when missing (same as the GUI).
    std::string resources_dir;
    std::string data_dir;
    // Scratch dir for temporary G-code; defaults to the system temp dir.
    std::string temp_dir;

    // Embedded: run work on the host main loop (e.g. wxTheApp->CallAfter) and tell
    // whether the calling thread is that loop.
    Dispatcher            host_dispatcher;
    std::function<bool()> host_is_current;
};

class Core
{
public:
    static Result<std::unique_ptr<Core>> create(CoreOptions options);
    ~Core();
    Core(const Core &)            = delete;
    Core &operator=(const Core &) = delete;

    const CoreOptions &options() const { return m_options; }
    bool               headless() const { return m_options.mode == CoreOptions::Mode::Headless; }

    EventBus &events() { return *m_events; }
    Executor &executor() { return *m_executor; }

    // Services (headless mode only; throw std::logic_error in embedded mode for now).
    PresetService  &presets();
    ProjectService &project();
    SliceService   &slicer();

    // Non-fatal issues found at startup (e.g. preset load substitutions).
    const std::vector<std::string> &startup_warnings() const { return m_startup_warnings; }

    static std::string version();     // print_bot core API version
    static std::string slicer_version(); // underlying slicer engine version

private:
    explicit Core(CoreOptions options);
    Status init();

    CoreOptions                     m_options;
    std::unique_ptr<EventBus>       m_events;
    std::unique_ptr<Executor>       m_executor;
    std::unique_ptr<PresetService>  m_presets;
    std::unique_ptr<ProjectService> m_project;
    std::unique_ptr<SliceService>   m_slicer;
    std::vector<std::string>        m_startup_warnings;
};

} // namespace printbot::core

#endif
