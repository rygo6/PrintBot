#include "obn/camera_probe.hpp"

#include <system_error>
// Deliberately no transport includes: the injected primitive carries all
// I/O (see camera_probe.hpp), so this TU compiles and links without the
// TLS dialer — that is what keeps the camera_probe_test link surface to
// camera_probe + camera_url + json_lite.
#include <thread>

namespace obn::camera_probe {

Result probe_both(const std::string& host, int ctrl_port, int video_port,
                  int timeout_ms, const ProbeFn& probe)
{
    Result r;
    if (video_port <= 0) {
        // No video port (MJPEG-only URL): one dial, identical to the
        // legacy single-port pick.
        r.ctrl = probe(host, ctrl_port, timeout_ms);
        return r;
    }

    bool ctrl = false;
    bool video = false;
    std::thread t_ctrl;
    try {
        t_ctrl = std::thread([&] { ctrl = probe(host, ctrl_port, timeout_ms); });
    } catch (const std::system_error&) {
        // Cannot spawn workers: run both probes inline (correctness over
        // concurrency) rather than dropping a result.
        r.ctrl  = probe(host, ctrl_port, timeout_ms);
        r.video = probe(host, video_port, timeout_ms);
        return r;
    }
    // First worker is running; from here on every path must still join it.
    try {
        std::thread t_video([&] { video = probe(host, video_port, timeout_ms); });
        t_video.join();
    } catch (const std::system_error&) {
        // Second worker would not start — run this port inline while the
        // first keeps running, then join below.
        video = probe(host, video_port, timeout_ms);
    }
    t_ctrl.join();

    r.ctrl  = ctrl;
    r.video = video;
    return r;
}

} // namespace obn::camera_probe
