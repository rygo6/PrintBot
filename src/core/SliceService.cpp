#include "SliceService.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include "Core.hpp"
#include "Json.hpp"
#include "PresetService.hpp"
#include "ProjectService.hpp"
#include "SliceEvents.hpp"

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace printbot::core {

struct SliceService::Job
{
    JobId                       id = 0;
    Model                       model;
    DynamicPrintConfig          config;
    bool                        bbl = false;
    std::vector<std::string>    filament_names;
    Revision                    project_rev = 0, presets_rev = 0;
    std::string                 gcode_path;
    std::atomic<bool>           cancel_requested{false};
    std::mutex                  warn_mutex;
    std::vector<SliceWarning>   warnings;
};

SliceService::SliceService(Core &core) : m_core(core), m_print(std::make_unique<Print>())
{
    fs::path base = core.options().temp_dir.empty() ? fs::temp_directory_path() : fs::path(core.options().temp_dir);
    m_temp_dir    = (base / fs::unique_path("print_bot_core_%%%%%%%%")).string();
    m_snapshot    = std::make_shared<SliceSnapshot>();
}

SliceService::~SliceService()
{
    shutdown();
    boost::system::error_code ec;
    fs::remove_all(m_temp_dir, ec);
}

void SliceService::shutdown()
{
    if (m_job) m_job->cancel_requested = true;
    if (m_print) m_print->cancel();
    if (m_worker.joinable()) m_worker.join();
}

std::shared_ptr<const SliceSnapshot> SliceService::snapshot() const
{
    std::lock_guard<std::mutex> lk(m_snap_mutex);
    // Outdated is derived: compare the inputs the result came from with the current revisions.
    auto snap = m_snapshot;
    if (snap->state == SliceSnapshot::State::Done || snap->state == SliceSnapshot::State::Running) {
        auto proj = const_cast<Core &>(m_core).project().snapshot();
        auto pres = const_cast<Core &>(m_core).presets().snapshot();
        bool outdated = (proj && proj->rev != snap->project_rev) || (pres && pres->rev != snap->presets_rev);
        if (outdated != snap->outdated) {
            auto copy      = std::make_shared<SliceSnapshot>(*snap);
            copy->outdated = outdated;
            return copy;
        }
    }
    return snap;
}

std::shared_ptr<SliceSnapshot> SliceService::mutable_copy() const
{
    std::lock_guard<std::mutex> lk(m_snap_mutex);
    return std::make_shared<SliceSnapshot>(*m_snapshot);
}

void SliceService::set_snapshot(std::shared_ptr<SliceSnapshot> s)
{
    nlohmann::json detail = to_json(*s);
    {
        std::lock_guard<std::mutex> lk(m_snap_mutex);
        m_snapshot = std::move(s);
    }
    m_snap_cv.notify_all();
    m_core.events().publish(Topic::Slice, slice_events::kState, std::move(detail));
}

std::shared_ptr<const SliceSnapshot> SliceService::wait(JobId job, double timeout_s) const
{
    std::unique_lock<std::mutex> lk(m_snap_mutex);
    const JobId target = job ? job : m_snapshot->job;
    m_snap_cv.wait_for(lk, std::chrono::milliseconds(int64_t(std::max(0.0, timeout_s) * 1000)), [&]() {
        return m_snapshot->job != target || m_snapshot->state != SliceSnapshot::State::Running;
    });
    lk.unlock();
    return snapshot();
}

Result<JobId> SliceService::start()
{
    return m_core.executor().call([&]() -> Result<JobId> {
        const Model &model = m_core.project().model();
        if (model.objects.empty()) return make_error("project.empty", "nothing to slice: load a model first");
        auto pres = m_core.presets().snapshot();
        if (!pres || pres->printer.empty()) return make_error("presets.none", "no printer preset selected");

        // Cancel and join a running job first (one slice at a time).
        if (m_worker.joinable()) {
            if (m_job) m_job->cancel_requested = true;
            m_print->cancel();
            m_worker.join();
        }

        auto job = std::make_shared<Job>();
        job->id  = ++m_next_job;
        job->model = model; // copy; the worker never touches live core state
        try {
            job->config = m_core.presets().full_config();
        } catch (const std::exception &ex) {
            return make_error("presets.invalid", std::string("could not build the print config: ") + ex.what());
        }
        job->bbl            = m_core.presets().is_bbl_printer();
        job->filament_names = pres->filaments;
        job->project_rev    = m_core.project().revision();
        job->presets_rev    = pres->rev;
        fs::create_directories(m_temp_dir);
        job->gcode_path = (fs::path(m_temp_dir) / ("plate_1_job" + std::to_string(job->id) + ".gcode")).string();
        m_job           = job;

        auto snap         = std::make_shared<SliceSnapshot>();
        snap->state       = SliceSnapshot::State::Running;
        snap->job         = job->id;
        snap->message     = "Slicing started";
        snap->project_rev = job->project_rev;
        snap->presets_rev = job->presets_rev;
        set_snapshot(snap);

        m_worker = std::thread([this, job]() { run_job(job); });
        return job->id;
    });
}

Status SliceService::cancel()
{
    return m_core.executor().call([&]() -> Status {
        if (!m_job || !m_worker.joinable()) return make_error("slice.not_running", "no slice is running");
        m_job->cancel_requested = true;
        m_print->cancel();
        return Ok{};
    });
}

void SliceService::run_job(std::shared_ptr<Job> job)
{
    using clock      = std::chrono::steady_clock;
    const auto t0    = clock::now();
    Print     &print = *m_print;
    auto       fail  = [&](SliceSnapshot::State st, Error err) {
        m_core.executor().post([this, job, st, err]() {
            if (!m_job || m_job->id != job->id) return;
            auto s      = mutable_copy();
            s->state    = st;
            s->message  = err.message;
            s->percent  = 0;
            if (st == SliceSnapshot::State::Error) s->error = err;
            s->summary.reset();
            set_snapshot(s);
        });
    };

    // Always detach the callback (it captures locals) however we leave.
    struct ResetCallback { Print &p; ~ResetCallback() { p.set_status_callback(nullptr); } } reset_cb{print};
    int last_percent = -1;
    print.set_status_callback([&, job](const PrintBase::SlicingStatus &st) {
        if ((st.flags & (PrintBase::SlicingStatus::UPDATE_PRINT_STEP_WARNINGS | PrintBase::SlicingStatus::UPDATE_PRINT_OBJECT_STEP_WARNINGS)) && !st.text.empty()) {
            std::lock_guard<std::mutex> lk(job->warn_mutex);
            job->warnings.push_back({st.warning_level == PrintStateBase::WarningLevel::CRITICAL ? "error" : "warning", "", st.text});
        }
        if (st.percent >= 0 && st.percent != last_percent) {
            last_percent = st.percent;
            m_core.events().publish(Topic::Slice, slice_events::kProgress, {{"job", job->id}, {"percent", st.percent}, {"text", st.text}});
            std::lock_guard<std::mutex> lk(m_snap_mutex);
            if (m_snapshot->job == job->id && m_snapshot->state == SliceSnapshot::State::Running) {
                auto s     = std::make_shared<SliceSnapshot>(*m_snapshot);
                s->percent = st.percent;
                s->message = st.text;
                m_snapshot = s;
            }
        }
    });

    try {
        print.restart();
        print.set_BBL_Printer(job->bbl);
        print.set_plate_origin(Vec3d::Zero());
        const size_t filament_count = std::max<size_t>(1, job->filament_names.size());
        print.apply(job->model, job->config);
        Model::setExtruderParams(job->config, int(filament_count));
        Model::setPrintSpeedTable(job->config, print.config());

        StringObjectException warning;
        StringObjectException err = print.validate(&warning);
        if (!err.string.empty()) return fail(SliceSnapshot::State::Error, make_error("slice.validate", err.string));
        if (!warning.string.empty()) {
            std::lock_guard<std::mutex> lk(job->warn_mutex);
            job->warnings.push_back({"warning", "validate", warning.string});
        }
        if (print.empty())
            return fail(SliceSnapshot::State::Error,
                        make_error("slice.empty", "nothing to slice: no object is fully inside the printable volume"));
        if (job->cancel_requested) throw CanceledException();

        print.process();
        GCodeProcessorResult result;
        std::string out = print.export_gcode(job->gcode_path, &result, nullptr);
        // Same post-processing as the GUI (user post_process scripts from the process preset).
        std::string out_name = out;
        run_post_process_scripts(out, false, "File", out_name, print.full_print_config());

        SliceSummary sum;
        using Mode       = PrintEstimatedStatistics::ETimeMode;
        const auto &mode = result.print_statistics.modes[size_t(Mode::Normal)];
        sum.print_time_s   = mode.time;
        sum.print_time     = get_time_dhms(mode.time);
        sum.prepare_time_s = mode.prepare_time;
        sum.layer_count    = int(mode.layers_times.size());
        const auto *diam = print.full_print_config().option<ConfigOptionFloats>("filament_diameter");
        const auto *dens = print.full_print_config().option<ConfigOptionFloats>("filament_density");
        const auto *cost = print.full_print_config().option<ConfigOptionFloats>("filament_cost");
        const auto *type = print.full_print_config().option<ConfigOptionStrings>("filament_type");
        for (const auto &[idx, volume] : result.print_statistics.total_volumes_per_extruder) {
            FilamentUse f;
            f.index      = int(idx) + 1;
            f.preset     = idx < job->filament_names.size() ? job->filament_names[idx] : std::string();
            f.type       = type && idx < type->values.size() ? type->values[idx] : std::string();
            const double d = diam && idx < diam->values.size() ? diam->values[idx] : 1.75;
            const double r = d / 2.0;
            f.used_mm    = volume / (M_PI * r * r);
            f.used_g     = volume * (dens && idx < dens->values.size() ? dens->values[idx] : 1.24) / 1000.0;
            sum.filament_used_mm += f.used_mm;
            sum.filament_used_g += f.used_g;
            sum.filament_cost += f.used_g / 1000.0 * (cost && idx < cost->values.size() ? cost->values[idx] : 0.0);
            sum.filaments.push_back(std::move(f));
        }
        for (const PrintObject *po : print.objects()) {
            ++sum.objects;
            if (!po->layers().empty()) sum.max_z = std::max(sum.max_z, double(po->layers().back()->print_z));
        }
        {
            std::lock_guard<std::mutex> lk(job->warn_mutex);
            sum.warnings = job->warnings;
        }
        for (const auto &w : result.warnings)
            sum.warnings.push_back({w.level >= 2 ? "error" : w.level == 1 ? "warning" : "info", w.error_code, w.msg});
        if (result.gcode_check_result.error_code != 0)
            sum.warnings.push_back({"error", "gcode_check", "G-code exceeds the printable area/height (code " +
                                                                std::to_string(result.gcode_check_result.error_code) + ")"});
        if (result.conflict_result)
            sum.warnings.push_back({"error", "path_conflict", "toolpath conflict between '" + result.conflict_result->_objName1 + "' and '" +
                                                                  result.conflict_result->_objName2 + "'"});
        sum.gcode_temp_path = out;
        sum.gcode_bytes     = fs::exists(out) ? uint64_t(fs::file_size(out)) : 0;
        sum.wall_time_s     = std::chrono::duration<double>(clock::now() - t0).count();
        auto done_config    = std::make_shared<const DynamicPrintConfig>(print.full_print_config());

        m_core.executor().post([this, job, sum, done_config]() {
            if (!m_job || m_job->id != job->id) return;
            auto s     = mutable_copy();
            s->state   = SliceSnapshot::State::Done;
            s->percent = 100;
            s->message = "Slicing complete";
            s->summary = sum;
            s->error.reset();
            {
                std::lock_guard<std::mutex> lk(m_snap_mutex);
                m_done_config = done_config;
            }
            set_snapshot(s);
        });
    } catch (const CanceledException &) {
        fail(SliceSnapshot::State::Cancelled, make_error("slice.cancelled", "slicing was cancelled"));
    } catch (const std::exception &ex) {
        if (job->cancel_requested) fail(SliceSnapshot::State::Cancelled, make_error("slice.cancelled", "slicing was cancelled"));
        else fail(SliceSnapshot::State::Error, make_error("slice.failed", ex.what()));
    }
}

Result<SliceService::ExportInfo> SliceService::export_gcode(const std::string &path, bool overwrite)
{
    auto snap = snapshot();
    if (snap->state != SliceSnapshot::State::Done || !snap->summary)
        return make_error("slice.not_done", std::string("no finished slice to export (state: ") + to_string(snap->state) + ")");
    if (snap->outdated) return make_error("slice.outdated", "the project or presets changed since the last slice; slice again");
    namespace sfs = std::filesystem;
    const sfs::path dst = sfs::absolute(sfs::path(path));
    if (sfs::exists(dst) && !overwrite) return make_error("file.exists", dst.string() + " already exists");
    if (!dst.parent_path().empty() && !sfs::exists(dst.parent_path())) return make_error("file.no_dir", "directory does not exist: " + dst.parent_path().string());
    std::shared_ptr<const DynamicPrintConfig> config;
    {
        std::lock_guard<std::mutex> lk(m_snap_mutex);
        config = m_done_config;
    }
    try {
        // Copy to a temp name in the destination dir, then rename (no half-written files).
        const sfs::path tmp = dst.string() + ".tmp";
        sfs::copy_file(snap->summary->gcode_temp_path, tmp, sfs::copy_options::overwrite_existing);
        sfs::rename(tmp, dst);
        // Same final step as the GUI export (only acts if gcode_add_line_number is enabled).
        if (config) gcode_add_line_number(dst.string(), *config);
    } catch (const std::exception &ex) {
        return make_error("file.write_failed", std::string("export failed: ") + ex.what());
    }
    ExportInfo info;
    info.path  = dst.string();
    info.bytes = uint64_t(sfs::file_size(dst));
    info.job   = snap->job;
    m_core.events().publish(Topic::Slice, "slice.exported", {{"path", info.path}, {"bytes", info.bytes}, {"job", info.job}});
    return info;
}

} // namespace printbot::core
