#pragma once

// CAM-01: concurrent two-port LAN camera reachability probe.
//
// Structure ported (shape only) from pandaproxy's camera detection:
// src/pandaproxy/camera/detection.py in gitlab.com/nerdycraft/pandaproxy
// (MIT) — "chamber_result, rtsp_result = await asyncio.gather(
// _probe_chamber_port(ip, ...), _probe_rtsp_port(ip, ...))" with an
// explicit both-answered policy. Our constraints differ from that
// reference on purpose:
//   * reachability-only — we qualify a LAN URL, we do not identify the
//     printer's protocol. pandaproxy's "both answered -> prefer chamber"
//     is an identification policy; ours is either-port-answers
//     (reachable() below, OQ3).
//   * TCP-level — the injected primitive wraps the existing dial-and-close
//     helper (abi_camera.cpp tcp_open -> obn::tls::dial); no TLS
//     handshake, no protocol probe.
//   * 400 ms budget — kTimeoutMs = 400 stays at its abi_camera.cpp site and
//     applies per port; the concurrent worst case is still 400 ms.
//   * offload-thread semantics — nothing in here ever runs on Studio's
//     calling thread; the detached work lambda in abi_camera.cpp keeps
//     carrying the work (see the probe comment at that site).

#include <functional>
#include <string>

namespace obn::camera_probe {

// Per-port reachability outcome of one probe_both call.
struct Result {
    bool ctrl  = false; // chamber CTRL port (:6000) answered
    bool video = false; // video port (:322 RTSPS / :554 RTSP) answered
};

// Reachability primitive in the shape of abi_camera.cpp's tcp_open helper:
// dial the port with a timeout, close, true when it answers. All I/O goes
// through this so camera_probe.cpp itself has zero network dependencies
// (that is what keeps the unit-test link surface small).
using ProbeFn = std::function<bool(const std::string& host, int port,
                                   int timeout_ms)>;

// Probes ctrl_port (chamber :6000) and video_port (RTSPS :322 / RTSP :554)
// CONCURRENTLY — one std::thread per port, both joined (repo threading
// idiom; pandaproxy gather shape as structure only). video_port <= 0 calls
// the primitive once for ctrl_port only (same single-dial behavior as the
// legacy pick). If thread construction fails with std::system_error the
// probes run inline instead — correctness over concurrency, never a
// dropped result.
Result probe_both(const std::string& host, int ctrl_port, int video_port,
                  int timeout_ms, const ProbeFn& probe);

// OQ3 policy: either port answers => the LAN URL is reachable. An
// RTSPS-only printer and a chamber-only printer each get an honest answer,
// and the concurrent worst case (400 ms) never exceeds today's single-port
// budget. Result wiring at the three abi_camera.cpp call sites stays one
// bool.
inline bool reachable(const Result& r) { return r.ctrl || r.video; }

} // namespace obn::camera_probe
