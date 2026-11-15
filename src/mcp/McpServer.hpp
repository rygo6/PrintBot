// print_bot MCP server: Model Context Protocol over stdio (newline-delimited JSON-RPC 2.0)
// on top of the GUI-free core. No wxWidgets.
#ifndef printbot_mcp_McpServer_hpp_
#define printbot_mcp_McpServer_hpp_

#include <cstdio>
#include <functional>
#include <istream>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace printbot::core {
class Core;
}

namespace printbot::mcp {

using nlohmann::json;

// Thrown by tool handlers; becomes an MCP tool result with isError = true.
struct ToolError
{
    std::string code;
    std::string message;
};

struct ToolContext
{
    core::Core                                         &core;
    // Report progress (0..100) to the client if it asked for progress notifications.
    std::function<void(double, const std::string &)> progress;
};

struct Tool
{
    std::string                                          name;
    std::string                                          title;
    std::string                                          description;
    json                                                 input_schema;
    bool                                                 read_only = true;
    std::function<json(ToolContext &, const json &args)> handler;
};

// Headless tool set (operates on the server's own in-process core).
std::vector<Tool> make_tools();
std::string print_approval_instructions();
// Instructions sent to clients by the headless server.
std::string headless_instructions();

class McpServer
{
public:
    // tools: the tool set to serve (headless: make_tools(); live GUI: the GUI's tool set).
    // instructions: the "instructions" string returned from initialize.
    McpServer(core::Core &core, std::FILE *out, std::vector<Tool> tools, std::string instructions);
    ~McpServer();
    // Reads requests until EOF; returns the process exit code.
    int run(std::istream &in);

private:
    void handle(const json &msg);
    void guarded_handle(const json &msg);
    void safe_reply_error(const json &id, int code, const std::string &message);
    void handle_call(json id, json params);
    void send(const json &msg);
    void reply(const json &id, json result);
    void reply_error(const json &id, int code, const std::string &message);

    core::Core            &m_core;
    std::FILE             *m_out;
    std::mutex             m_out_mutex;
    std::vector<Tool>      m_tools;
    std::mutex             m_threads_mutex;
    std::list<std::thread> m_threads;
    std::string            m_protocol_version;
    std::string            m_instructions;
};

} // namespace printbot::mcp

#endif
