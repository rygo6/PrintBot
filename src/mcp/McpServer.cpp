#include "McpServer.hpp"

#include <algorithm>
#include <exception>

#include "core/Core.hpp"
#include "core/Version.hpp"

namespace printbot::mcp {

namespace {
const char *kSupportedVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

} // namespace

std::string print_approval_instructions()
{
    return "An explicit human instruction such as 'Print this' or 'Go ahead and now print' authorizes exactly one print "
           "of the identified job. Do not ask again in chat or require an approval popup. Multiple confirmations of the same "
           "pending job do not authorize multiple prints. Before sending, complete Sync Info and filament synchronization, "
           "verify printer readiness and native feeder labels such as HT-A, prepare and slice the requested plate, then run "
           "print_plate dry_run=true and inspect the actual Print dialog mapping. Report the job, printer, material, mapping, "
           "time and usage as information, not another approval question. Call print_plate with dry_run=false, fingerprint, "
           "user_authorized=true and authorization_id. Use a stable unique ID for the human request (conversation/message ID "
           "or a recorded UUID), containing only ASCII letters, digits, hyphens or underscores. Reuse it across retries, "
           "reconnections and restarts; never derive it from job facts or mint another for the same request. The server persists "
           "consumption before sending and rejects reuse with approval.used. A failed or uncertain send must never trigger "
           "another print automatically. New prints need a new explicit human instruction. Printer control must be enabled "
           "with human authorization. Changes outside the requested scope or ambiguous jobs still require clarification. "
           "Fingerprints and final transport checks reject changed job, printer, mapping or settings. Print/Send controls remain "
           "unavailable to generic dialog tools; use print_plate. Poll get_print_operation and report started only when the "
           "printer reports the matching job. The server trusts the agent to attest the human request; an ID is replay protection, "
           "not independent authentication of chat intent. The headless server has no print tools and never sends.";
}

std::string headless_instructions()
{
    return "print_bot is a LAN-only Bambu Lab slicer. This is the HEADLESS server (no print_bot window is running, or "
           "--headless was given): it slices in its own process and nothing is shown on screen. Typical flow: "
           "list_printer_presets -> select_presets (printer, then process/filaments) -> load_model (STL/3MF/OBJ/STEP path) -> "
           "slice -> export_gcode(path). get_status shows the current state; get_events polls state-change events. "
           "Printers, plates, print jobs and UI control need the running print_bot app (start it, then restart this server). " +
           print_approval_instructions();
}

McpServer::McpServer(core::Core &core, std::FILE *out, std::vector<Tool> tools, std::string instructions)
    : m_core(core), m_out(out), m_tools(std::move(tools)), m_instructions(std::move(instructions))
{}

McpServer::~McpServer()
{
    std::lock_guard<std::mutex> lk(m_threads_mutex);
    for (auto &t : m_threads)
        if (t.joinable()) t.join();
}

void McpServer::send(const json &msg)
{
    const std::string line = msg.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> lk(m_out_mutex);
    std::fwrite(line.data(), 1, line.size(), m_out);
    std::fflush(m_out);
}

void McpServer::reply(const json &id, json result) { send(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}); }

void McpServer::reply_error(const json &id, int code, const std::string &message)
{
    send(json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

int McpServer::run(std::istream &in)
{
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        json msg;
        try {
            msg = json::parse(line);
        } catch (const std::exception &ex) {
            safe_reply_error(nullptr, -32700, std::string("parse error: ") + ex.what());
            continue;
        }
        if (msg.is_array()) {
            if (msg.empty()) safe_reply_error(nullptr, -32600, "invalid request: empty batch");
            for (const auto &m : msg) guarded_handle(m);
        } else {
            guarded_handle(msg);
        }
    }
    // EOF: let in-flight tool calls finish and answer.
    std::lock_guard<std::mutex> lk(m_threads_mutex);
    for (auto &t : m_threads)
        if (t.joinable()) t.join();
    m_threads.clear();
    return 0;
}

namespace {
// JSON-RPC ids are strings or numbers (MCP forbids null ids on requests).
bool valid_id(const json &id) { return id.is_string() || id.is_number_integer() || id.is_number_unsigned(); }
} // namespace

// Outer boundary for one message: nothing a client sends may escape as an exception.
void McpServer::guarded_handle(const json &msg)
{
    json id = nullptr;
    try {
        if (msg.is_object() && msg.contains("id") && valid_id(msg["id"])) id = msg["id"];
        handle(msg);
    } catch (const std::exception &e) {
        safe_reply_error(id, -32603, std::string("internal error: ") + e.what());
    } catch (...) {
        safe_reply_error(id, -32603, "internal error");
    }
}

void McpServer::safe_reply_error(const json &id, int code, const std::string &message)
{
    try {
        reply_error(id, code, message);
    } catch (...) {
        // Output closed or unwritable: nothing more to do for this message.
    }
}

void McpServer::handle(const json &msg)
{
    if (!msg.is_object()) {
        reply_error(nullptr, -32600, "invalid request: a JSON-RPC message must be an object");
        return;
    }
    const bool has_id = msg.contains("id");
    if (has_id && !valid_id(msg["id"])) {
        reply_error(nullptr, -32600, "invalid request: 'id' must be a string or an integer");
        return;
    }
    const json id = has_id ? msg["id"] : json(nullptr);
    if (!msg.contains("method")) {
        // A response to a server->client request (none are sent), or junk.
        if (has_id && !msg.contains("result") && !msg.contains("error")) reply_error(id, -32600, "invalid request: missing 'method'");
        return;
    }
    if (!msg.contains("jsonrpc") || !msg["jsonrpc"].is_string() || msg["jsonrpc"].get<std::string>() != "2.0") {
        if (has_id) reply_error(id, -32600, "invalid request: 'jsonrpc' must be \"2.0\"");
        return;
    }
    if (!msg["method"].is_string()) {
        if (has_id) reply_error(id, -32600, "invalid request: 'method' must be a string");
        return;
    }
    const std::string method = msg["method"].get<std::string>();
    if (msg.contains("params") && !msg["params"].is_object()) {
        if (has_id) reply_error(id, -32602, "invalid params: 'params' must be an object");
        return;
    }
    const json params = msg.contains("params") ? msg["params"] : json::object();

    if (!has_id) return; // notifications: initialized, cancelled, ... nothing to do

    if (method == "initialize") {
        std::string requested;
        if (params.contains("protocolVersion")) {
            if (!params["protocolVersion"].is_string()) {
                reply_error(id, -32602, "invalid params: 'protocolVersion' must be a string");
                return;
            }
            requested = params["protocolVersion"].get<std::string>();
        }
        m_protocol_version = kSupportedVersions[0];
        for (const char *v : kSupportedVersions)
            if (requested == v) m_protocol_version = v;
        reply(id, json{{"protocolVersion", m_protocol_version},
                       {"capabilities", {{"tools", {{"listChanged", false}}}}},
                       {"serverInfo", {{"name", "print_bot"}, {"title", "print_bot (LAN-only Bambu slicer)"}, {"version", PRINTBOT_CORE_VERSION}}},
                       {"instructions", m_instructions}});
    } else if (method == "ping") {
        reply(id, json::object());
    } else if (method == "tools/list") {
        json tools = json::array();
        for (const Tool &t : m_tools)
            tools.push_back(json{{"name", t.name},
                                 {"title", t.title},
                                 {"description", t.description},
                                 {"inputSchema", t.input_schema},
                                 {"annotations", {{"title", t.title}, {"readOnlyHint", t.read_only}, {"openWorldHint", false}}}});
        reply(id, json{{"tools", tools}});
    } else if (method == "tools/call") {
        // Validate before dispatch so a malformed call never reaches a tool worker.
        if (!params.contains("name") || !params["name"].is_string() || params["name"].get<std::string>().empty()) {
            reply_error(id, -32602, "invalid params: 'name' must be a non-empty string");
            return;
        }
        if (params.contains("arguments") && !params["arguments"].is_object() && !params["arguments"].is_null()) {
            reply_error(id, -32602, "invalid params: 'arguments' must be an object");
            return;
        }
        if (params.contains("_meta") && !params["_meta"].is_object()) {
            reply_error(id, -32602, "invalid params: '_meta' must be an object");
            return;
        }
        const std::string name = params["name"].get<std::string>();
        if (std::find_if(m_tools.begin(), m_tools.end(), [&](const Tool &t) { return t.name == name; }) == m_tools.end()) {
            reply_error(id, -32602, "unknown tool: " + name);
            return;
        }
        // Run each call on its own thread so long calls (slice) don't block ping or cancel_slice.
        std::lock_guard<std::mutex> lk(m_threads_mutex);
        m_threads.emplace_back([this, id, params]() {
            // Worker boundary: an exception here must never reach std::terminate.
            try {
                handle_call(id, params);
            } catch (const std::exception &e) {
                safe_reply_error(id, -32603, std::string("internal error: ") + e.what());
            } catch (...) {
                safe_reply_error(id, -32603, "internal error");
            }
        });
    } else if (method == "resources/list") {
        reply(id, json{{"resources", json::array()}});
    } else if (method == "resources/templates/list") {
        reply(id, json{{"resourceTemplates", json::array()}});
    } else if (method == "prompts/list") {
        reply(id, json{{"prompts", json::array()}});
    } else if (method == "logging/setLevel") {
        reply(id, json::object());
    } else {
        reply_error(id, -32601, "method not found: " + method);
    }
}

void McpServer::handle_call(json id, json params)
{
    // Types were validated in handle().
    const std::string name = params["name"].get<std::string>();
    json              args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
    json              token;
    if (params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains("progressToken")) {
        const json &t = params["_meta"]["progressToken"];
        if (t.is_string() || t.is_number_integer() || t.is_number_unsigned()) token = t;
    }

    auto it = std::find_if(m_tools.begin(), m_tools.end(), [&](const Tool &t) { return t.name == name; });
    if (it == m_tools.end()) {
        reply_error(id, -32602, "unknown tool: " + name);
        return;
    }
    ToolContext ctx{m_core, [this, token](double progress, const std::string &message) {
                        if (token.is_null()) return;
                        try {
                            send(json{{"jsonrpc", "2.0"},
                                      {"method", "notifications/progress"},
                                      {"params", {{"progressToken", token}, {"progress", progress}, {"total", 100}, {"message", message}}}});
                        } catch (...) {}
                    }};
    json result;
    try {
        json out = it->handler(ctx, args);
        result   = json{{"content", json::array({json{{"type", "text"}, {"text", out.dump(2, ' ', false, json::error_handler_t::replace)}}})}, {"structuredContent", out}, {"isError", false}};
    } catch (const ToolError &e) {
        json err = json{{"error", {{"code", e.code}, {"message", e.message}}}};
        result   = json{{"content", json::array({json{{"type", "text"}, {"text", e.code + ": " + e.message}}})}, {"structuredContent", err}, {"isError", true}};
    } catch (const std::exception &e) {
        json err = json{{"error", {{"code", "internal"}, {"message", e.what()}}}};
        result   = json{{"content", json::array({json{{"type", "text"}, {"text", std::string("internal error: ") + e.what()}}})}, {"structuredContent", err}, {"isError", true}};
    } catch (...) {
        json err = json{{"error", {{"code", "internal"}, {"message", "unknown exception"}}}};
        result   = json{{"content", json::array({json{{"type", "text"}, {"text", "internal error"}}})}, {"structuredContent", err}, {"isError", true}};
    }
    reply(id, std::move(result));
}

} // namespace printbot::mcp
