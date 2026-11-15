#include "Core.hpp"

#include <stdexcept>

#include <boost/filesystem.hpp>

#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include "PresetService.hpp"
#include "ProjectService.hpp"
#include "SliceService.hpp"
#include "Version.hpp"

namespace fs = boost::filesystem;

namespace printbot::core {

std::string Core::version() { return PRINTBOT_CORE_VERSION; }
std::string Core::slicer_version() { return SLIC3R_VERSION; }

Core::Core(CoreOptions options) : m_options(std::move(options)), m_events(std::make_unique<EventBus>()) {}

Result<std::unique_ptr<Core>> Core::create(CoreOptions options)
{
    std::unique_ptr<Core> core(new Core(std::move(options)));
    Status st = core->init();
    if (!st) return st.error();
    return std::move(core);
}

Status Core::init()
{
    if (m_options.mode == CoreOptions::Mode::Embedded) {
        // Phase 1: the GUI still owns presets/model; the core thread is the host main loop.
        m_executor = std::make_unique<DispatchExecutor>(m_options.host_dispatcher, m_options.host_is_current);
        return Ok{};
    }

    if (m_options.resources_dir.empty() || !fs::exists(fs::path(m_options.resources_dir) / "profiles"))
        return make_error("core.resources", "resources dir not found or has no profiles/: '" + m_options.resources_dir + "'");
    if (m_options.data_dir.empty()) return make_error("core.data_dir", "no data dir given");

    // libslic3r keeps these as process globals (the headless core is the only user in its process).
    const fs::path res = fs::absolute(m_options.resources_dir);
    Slic3r::set_resources_dir(res.string());
    Slic3r::set_var_dir((res / "images").string());
    Slic3r::set_local_dir((res / "i18n").string());
    Slic3r::set_sys_shapes_dir((res / "shapes").string());
    boost::system::error_code ec;
    fs::create_directories(m_options.data_dir, ec);
    Slic3r::set_data_dir(fs::absolute(m_options.data_dir).string());
    Slic3r::set_temporary_dir(m_options.temp_dir.empty() ? fs::temp_directory_path().string() : m_options.temp_dir);

    m_executor = std::make_unique<ThreadExecutor>("printbot-core");
    m_presets  = std::make_unique<PresetService>(*this);
    Status st  = m_executor->call([this]() { return m_presets->load(m_startup_warnings); });
    if (!st) return st;
    m_project = m_executor->call([this]() { return std::make_unique<ProjectService>(*this); });
    m_slicer  = std::make_unique<SliceService>(*this);
    return Ok{};
}

Core::~Core()
{
    // Order matters: join the slice worker, then drain the core thread (it may still hold
    // completion tasks for the services), then destroy the services.
    if (m_slicer) m_slicer->shutdown();
    if (auto *te = dynamic_cast<ThreadExecutor *>(m_executor.get())) te->stop();
    m_slicer.reset();
    m_project.reset();
    m_presets.reset();
}

PresetService &Core::presets()
{
    if (!m_presets) throw std::logic_error("presets service is not available in embedded mode yet");
    return *m_presets;
}

ProjectService &Core::project()
{
    if (!m_project) throw std::logic_error("project service is not available in embedded mode yet");
    return *m_project;
}

SliceService &Core::slicer()
{
    if (!m_slicer) throw std::logic_error("slice service is not available in embedded mode yet");
    return *m_slicer;
}

} // namespace printbot::core
