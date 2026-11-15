// print_bot core: headless slicing + G-code export for the current plate.
// Inputs (model copy + full config) are captured on the core thread; the slice runs on
// a worker thread and reports back through the core thread and Topic::Slice events.
// GUI-free (no wxWidgets).
#ifndef printbot_core_SliceService_hpp_
#define printbot_core_SliceService_hpp_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Result.hpp"
#include "Snapshots.hpp"

namespace Slic3r {
class Print;
class DynamicPrintConfig;
}

namespace printbot::core {

class Core;

class SliceService
{
public:
    struct ExportInfo
    {
        std::string path;
        uint64_t    bytes = 0;
        JobId       job   = 0;
    };

    explicit SliceService(Core &core);
    ~SliceService();

    // --- thread-safe API ---
    // Start slicing the current project with the current presets. Cancels a running job.
    Result<JobId>                         start();
    std::shared_ptr<const SliceSnapshot>  snapshot() const;
    // Block until `job` (0 = current) leaves the Running state or timeout_s elapses.
    std::shared_ptr<const SliceSnapshot>  wait(JobId job, double timeout_s) const;
    Status                                cancel();
    // Copy the G-code of the last successful, up-to-date slice to `path`.
    Result<ExportInfo>                    export_gcode(const std::string &path, bool overwrite);

    // Join the worker (called by Core before the core thread stops).
    void shutdown();

private:
    struct Job;
    void run_job(std::shared_ptr<Job> job);
    void set_snapshot(std::shared_ptr<SliceSnapshot> s);
    std::shared_ptr<SliceSnapshot> mutable_copy() const;

    Core                                 &m_core;
    std::unique_ptr<Slic3r::Print>        m_print; // worker-thread only
    std::thread                           m_worker;
    std::shared_ptr<Job>                  m_job;
    JobId                                 m_next_job = 0;
    mutable std::mutex                    m_snap_mutex;
    mutable std::condition_variable       m_snap_cv;
    std::shared_ptr<const SliceSnapshot>  m_snapshot;
    std::shared_ptr<const Slic3r::DynamicPrintConfig> m_done_config; // config of the last finished slice
    std::string                           m_temp_dir;
};

} // namespace printbot::core

#endif
