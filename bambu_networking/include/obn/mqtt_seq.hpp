#pragma once

// MQTT sequence_id for frames the plugin builds itself (project_file,
// app_cert_install, …). Stock 02.05.00–02.08.02 stays inside Studio's
// 20000–29999 window (DevUtil::is_studio_cmd) rather than using epoch-ms;
// project_file is always "20001" on a fresh process, app_cert_install is
// a random value in the same range. Reusing a constant across process
// restarts makes firmware reject the frame with
// `mqtt message verify failed` / err_code 84033544, and epoch-ms trips
// the AMS signed-32-bit hang, so we seed randomly once and increment.
// See research/06.02-mqtt.md and research/08.08-print-abi.md §8.8.1.

#include <atomic>
#include <cstdint>
#include <random>
#include <string>

#include "obn/config.hpp"
#include "obn/json_lite.hpp"

namespace obn {

inline std::string next_mqtt_seq_id()
{
    static std::atomic<std::uint32_t> n{[] {
        std::random_device rd;
        return static_cast<std::uint32_t>(rd());
    }()};
    const std::uint32_t v = n.fetch_add(1, std::memory_order_relaxed);
    return std::to_string(20000 + (v % 10000));
}

// EXP-01 flag-aware emission helpers for `sequence_id`, used ONLY at the four
// frames the plugin builds AND signs: print_job.cpp project_file and agent.cpp
// rescue_cloud_project_file / rescue_cloud_liveview / liveview prepare. They
// read obn::config::current().exp_numeric_sequence_id at call time; before any
// config load that is the default Settings (flag false), i.e. legacy behavior.
//
// Byte-identity argument for the flag-off form: next_mqtt_seq_id() returns
// std::to_string(20000 + (v % 10000)) -- pure digits, a domain on which
// json_escape (print_job.cpp:114-118) only adds the two surrounding quotes
// (no byte in 0-9 needs escaping), so the quote-wrap below is byte-identical
// to the legacy emission for this value range.
//
// next_mqtt_seq_id() itself is unchanged and stays the sole value source; the
// helpers are inline, so TUs that never call them emit no reference to
// obn::config::current() and every existing test link set keeps working.

// Ready-to-insert JSON literal: quoted string when the flag is off (legacy
// wire form), bare digits when it is on.
inline std::string seq_json_literal(const std::string& seq)
{
    if (obn::config::current().exp_numeric_sequence_id) return seq;
    return "\"" + seq + "\"";
}

// obn::json::Value twin for the Value(...) emission sites: Kind::String when
// the flag is off (exactly today's Value(std::string) behavior), Kind::Number
// when it is on (20000-29999 are exact in double, and json_lite.cpp:294-307
// dumps integral doubles as plain decimals, never `20001.0`).
inline obn::json::Value seq_json_value(const std::string& seq)
{
    if (obn::config::current().exp_numeric_sequence_id)
        return obn::json::Value(std::stod(seq));
    return obn::json::Value(seq);
}

} // namespace obn
