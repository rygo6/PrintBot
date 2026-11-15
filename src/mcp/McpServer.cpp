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
    return "Before preparing EVERY print approval, use live ui_open_dialog with dialog=sync_info for the sidebar Sync Info action, "
           "then complete Continue to sync filaments (or ui_open_dialog with dialog=sync_filaments if that prompt disappears). "
           "Inspect and resolve the synchronization dialogs, verify the resulting filament presets and printer status, and slice "
           "again if settings changed. Opening the workflow alone is not proof of completed sync. Identify the material source "
           "using the printer status tray label, such as HT-A or A1, rather than calling every source slot 1. "
           "Before EVERY individual print, run print_plate with dry_run=true and present the exact job to the human (project/file, "
           "plate, object names and copy counts, destination printer name/model and a distinguishing identifier, filament/material "
           "and mapping, estimated duration and material use), and ask whether to start that specific job on that specific printer. "
           "Only after an affirmative reply call print_plate with dry_run=false and that dry run's job_fingerprint. print_bot then "
           "shows the job in its own approval dialog; only the human's click there approves the send, agents cannot see or answer "
           "it, and every MCP call is refused while it is open. Enabling MCP printer control, an earlier approval, a general "
           "instruction to print or any argument an agent passes is not approval. Approval is single-use and bound to the job "
           "fingerprint: changed job contents/settings/material/printer/mapping return approval.stale, and repeat prints and retries "
           "need a new summary and approval. The Print and Send dialogs are human-only: ui_dialog_click and ui_dialog_set are "
           "refused while one is open. print_plate returns status send_requested with an operation_id, which is not proof of "
           "printing: poll get_print_operation and report printing as started only when it reports printing (the printer reported "
           "the matching job). If an outcome is unknown or uncertain, inspect printer status and ask the human; never resend "
           "automatically. The headless server has no print tools and can never send a print.";
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
