// Framework: repo convention (int main() + CHECK, first failure returns 1).
//
// CAM-01 unit test for obn::camera_probe — no sockets, ever: the injected
// probe primitive is a fake that records every (host, port, timeout) triple
// it receives and sleeps per script, so dial shape, the reachability
// matrix, the concurrency wall clock and idempotency are all deterministic.

#include "obn/camera_probe.hpp"
#include "obn/camera_url.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

using Clock = std::chrono::steady_clock;

struct Dial {
    std::string host;
    int         port;
    int         timeout_ms;

    bool operator==(const Dial& o) const
    {
        return host == o.host && port == o.port && timeout_ms == o.timeout_ms;
    }
};

// Mutex-guarded shared dial log: probe_both records from two threads at
// once, so a plain vector would race.
struct DialLog {
    mutable std::mutex mu;
    std::vector<Dial>  dials;

    void record(const std::string& host, int port, int timeout_ms)
    {
        std::lock_guard<std::mutex> lock(mu);
        dials.push_back(Dial{host, port, timeout_ms});
    }

    std::vector<Dial> take()
    {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<Dial> out;
        out.swap(dials);
        return out;
    }
};

long long ms_since(Clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - t0)
        .count();
}

} // namespace

// (a) lv=rtsps URL dials exactly :6000 and :322 with the passed timeout.
static int test_rtsps_url_dials_both_ports()
{
    DialLog log;
    obn::camera::LocalCameraUrl u;
    CHECK(obn::camera::parse_local_camera_url(
        "bambu:///local/10.13.1.30?port=6000&lv=rtsps", u));
    CHECK(u.ctrl_port == 6000);
    CHECK(u.video_port == 322);

    const obn::camera_probe::ProbeFn probe =
        [&log](const std::string& host, int port, int timeout_ms) {
            log.record(host, port, timeout_ms);
            return true;
        };
    const obn::camera_probe::Result r =
        obn::camera_probe::probe_both(u.ip, u.ctrl_port, u.video_port, 400,
                                      probe);
    CHECK(obn::camera_probe::reachable(r));

    std::vector<Dial> dials = log.take();
    CHECK(dials.size() == 2);
    std::sort(dials.begin(), dials.end(),
              [](const Dial& a, const Dial& b) { return a.port < b.port; });
    CHECK(dials[0].host == "10.13.1.30");
    CHECK(dials[0].port == 322);
    CHECK(dials[0].timeout_ms == 400);
    CHECK(dials[1].host == "10.13.1.30");
    CHECK(dials[1].port == 6000);
    CHECK(dials[1].timeout_ms == 400);
    return 0;
}

// (b) URL without lv parses to video_port 0 and probes only :6000.
static int test_no_lv_url_dials_only_ctrl()
{
    DialLog log;
    obn::camera::LocalCameraUrl u;
    CHECK(obn::camera::parse_local_camera_url("bambu:///local/192.168.1.5.?port=6000",
                                              u));
    CHECK(u.video_port == 0);

    const obn::camera_probe::ProbeFn probe =
        [&log](const std::string& host, int port, int timeout_ms) {
            log.record(host, port, timeout_ms);
            return true;
        };
    const obn::camera_probe::Result r =
        obn::camera_probe::probe_both(u.ip, u.ctrl_port, u.video_port, 400,
                                      probe);
    CHECK(obn::camera_probe::reachable(r));

    const std::vector<Dial> dials = log.take();
    CHECK(dials.size() == 1);
    CHECK(dials[0].host == "192.168.1.5");
    CHECK(dials[0].port == 6000);
    CHECK(dials[0].timeout_ms == 400);
    return 0;
}

// (c) reachability matrix: ctrl-only reachable, rtsp-only reachable,
// neither unreachable (OQ3: either port answers).
static int test_reachability_matrix()
{
    const auto run = [](bool ctrl_answers, bool video_answers) {
        const obn::camera_probe::ProbeFn probe =
            [ctrl_answers, video_answers](const std::string&, int port, int) {
                return port == 6000 ? ctrl_answers : video_answers;
            };
        return obn::camera_probe::probe_both("10.13.1.30", 6000, 322, 400,
                                             probe);
    };

    {
        const obn::camera_probe::Result r = run(true, false);
        CHECK(r.ctrl);
        CHECK(!r.video);
        CHECK(obn::camera_probe::reachable(r)); // ctrl-only => reachable
    }
    {
        const obn::camera_probe::Result r = run(false, true);
        CHECK(!r.ctrl);
        CHECK(r.video);
        CHECK(obn::camera_probe::reachable(r)); // rtsp-only => reachable
    }
    {
        const obn::camera_probe::Result r = run(false, false);
        CHECK(!r.ctrl);
        CHECK(!r.video);
        CHECK(!obn::camera_probe::reachable(r)); // neither => unreachable
    }
    return 0;
}

// (e) idempotency: two identical probe_both calls yield identical Results
// and byte-identical recorded (host, port, timeout) triples — the probe
// holds no cross-call state.
static int test_idempotent()
{
    DialLog log;
    const obn::camera_probe::ProbeFn probe =
        [&log](const std::string& host, int port, int timeout_ms) {
            log.record(host, port, timeout_ms);
            return port == 6000; // ctrl answers, video does not
        };

    const obn::camera_probe::Result r1 =
        obn::camera_probe::probe_both("10.13.1.30", 6000, 322, 400, probe);
    const obn::camera_probe::Result r2 =
        obn::camera_probe::probe_both("10.13.1.30", 6000, 322, 400, probe);

    CHECK(r1.ctrl == r2.ctrl);
    CHECK(r1.video == r2.video);
    CHECK(r1.ctrl);
    CHECK(!r1.video);

    const std::vector<Dial> dials = log.take();
    CHECK(dials.size() == 4); // both ports exactly once, per call
    std::vector<Dial> first(dials.begin(), dials.begin() + 2);
    std::vector<Dial> second(dials.begin() + 2, dials.end());
    const auto by_port = [](const Dial& a, const Dial& b) {
        return a.port < b.port;
    };
    std::sort(first.begin(), first.end(), by_port);
    std::sort(second.begin(), second.end(), by_port);
    CHECK(first == second);
    return 0;
}

// (d) concurrency wall-clock: 400 ms per scripted port. The concurrent
// gather must finish in under 600 ms while the same two scripted probes
// taken sequentially cost at least 800 ms. Prints the machine-readable
// timing line recorded by the plan.
static int test_concurrent_faster_than_sequential()
{
    DialLog log;
    const obn::camera_probe::ProbeFn sleepy =
        [&log](const std::string& host, int port, int timeout_ms) {
            log.record(host, port, timeout_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            return true;
        };

    // single_port: today's one-dial design (video_port=0) measured under
    // identical fake-probe conditions.
    obn::camera::LocalCameraUrl u;
    CHECK(obn::camera::parse_local_camera_url("bambu:///local/10.13.1.30?port=6000",
                                              u));
    Clock::time_point t0 = Clock::now();
    const obn::camera_probe::Result single =
        obn::camera_probe::probe_both(u.ip, u.ctrl_port, u.video_port, 400,
                                      sleepy);
    const long long single_port = ms_since(t0);
    CHECK(obn::camera_probe::reachable(single));

    // sequential reference: the same two scripted probes back-to-back —
    // what a serial two-port design would have cost.
    t0 = Clock::now();
    const bool seq_ctrl  = sleepy("10.13.1.30", 6000, 400);
    const bool seq_video = sleepy("10.13.1.30", 322, 400);
    const long long sequential_reference = ms_since(t0);
    CHECK(seq_ctrl);
    CHECK(seq_video);
    CHECK(sequential_reference >= 800);

    // concurrent: both ports inside one probe_both call.
    t0 = Clock::now();
    const obn::camera_probe::Result conc =
        obn::camera_probe::probe_both("10.13.1.30", 6000, 322, 400, sleepy);
    const long long concurrent = ms_since(t0);
    CHECK(conc.ctrl);
    CHECK(conc.video);
    CHECK(concurrent < 600);

    std::cout << "probe timing: single_port=" << single_port
              << "ms sequential_reference=" << sequential_reference
              << "ms concurrent=" << concurrent << "ms\n";
    return 0;
}

int main()
{
    if (test_rtsps_url_dials_both_ports() != 0) return 1;
    if (test_no_lv_url_dials_only_ctrl() != 0) return 1;
    if (test_reachability_matrix() != 0) return 1;
    if (test_idempotent() != 0) return 1;
    if (test_concurrent_faster_than_sequential() != 0) return 1;

    std::cout << "camera_probe_test: ok\n";
    return 0;
}
