// print_bot: the running app's MCP endpoint ("live mode").
//
// The app listens on a private Unix domain socket (see src/mcp/McpSocket.hpp); print_bot-mcp
// bridges an MCP client's stdio to it. Each tool call runs its GUI work on the wx main thread
// (CallAfter + wait), through the same code paths the user's clicks take, so the window shows
// everything the agent does: models appear, presets switch, slicing progresses, tabs change.
// View-only state (tab, camera, selection, dialogs) is driven through the ui_* tools.
#ifndef slic3r_GUI_PrintBotMcp_hpp_
#define slic3r_GUI_PrintBotMcp_hpp_

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace printbot::mcp {
struct Tool;
}

namespace Slic3r { namespace GUI { namespace pbmcp {

// Start/stop the endpoint (GUI_App calls these after the main window is up / on exit).
void start_host();
void stop_host();
// Socket path the endpoint listens on ("" when not running).
std::string host_socket_path();

// Tool set and instructions served to live clients (PrintBotMcpTools.cpp).
std::vector<printbot::mcp::Tool> make_gui_tools();
std::string                      gui_instructions();

// Runs fn on the wx main thread and waits for its result (runs inline when already there).
// Throws ToolError "ui.blocked_by_dialog" when a modal dialog keeps the main thread from
// finishing (the action then completes once the dialog is answered), "ui.timeout" after
// timeout_s, and rethrows exceptions from fn.
nlohmann::json run_on_main(std::function<nlohmann::json()> fn, double timeout_s = 120.0);
// Re-entrancy policy for run_on_main calls made by the current (worker) thread. A queued call
// is executed by whatever event loop the main thread is running, including the nested loop of
// a modal dialog. Unless the tool may nest (read-only tools and the dialog tools that answer
// such dialogs), run_on_main refuses with "ui.blocked_by_dialog" while a modal dialog is shown
// and with "ui.busy" while another MCP call is still running on the main thread.
void set_tool_may_nest(bool may_nest);
// Posts fn to the main thread without waiting.
void post_to_main(std::function<void()> fn);

// Shows what the agent is doing in the app (a notification on the 3D canvas).
void agent_activity(const std::string &text);

// Printer control and print start are disabled unless the user enabled them: environment
// PRINTBOT_MCP_ALLOW_PRINTER_CONTROL=1, or "mcp_allow_printer_control = true" in the app config.
bool printer_control_allowed();

}}} // namespace Slic3r::GUI::pbmcp

#endif
