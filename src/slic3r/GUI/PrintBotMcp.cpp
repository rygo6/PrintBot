#include "PrintBotMcp.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>

#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/menu.h>
#include <wx/thread.h>
#include <wx/toplevel.h>

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "GLCanvas3D.hpp"
#include "NotificationManager.hpp"
#include "libslic3r/AppConfig.hpp"

#include "core/Core.hpp"
#include "mcp/McpServer.hpp"
#include "mcp/McpSocket.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

// Defined in wxWidgets (wincmn.cpp); non-null while wxWindow::PopupMenu() is tracking a context menu.
extern wxMenu *wxCurrentPopupMenu;

namespace Slic3r { namespace GUI { namespace pbmcp {

using printbot::mcp::ToolError;
using nlohmann::json;

namespace {

std::atomic<bool> g_exiting{false};
// Intentionally leaked: connection threads may outlive stop_host() while the app exits.
printbot::mcp::SocketListener *g_listener = nullptr;
std::atomic<int>               g_clients{0};

// Number of MCP calls currently executing on the main thread (> 0 means a new call would
// run nested inside another call's modal event loop).
int                      g_main_depth = 0;
thread_local bool        t_tool_may_nest = true;

std::vector<std::string> shown_modal_titles()
{
    std::vector<std::string> titles;
    for (wxWindow *w : wxTopLevelWindows) {
        auto *d = dynamic_cast<wxDialog *>(w);
        if (d && d->IsShown() && d->IsModal()) titles.push_back(d->GetTitle().ToUTF8().data());
    }
    return titles;
}

struct MainCall
{
    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
    json                    result;
    std::exception_ptr      ex;
};

// Titles of modal dialogs currently shown, asked from the main thread (which keeps serving
// CallAfter while it runs a modal loop). Empty if the main thread didn't answer in time.
std::vector<std::string> probe_modal_dialogs(double timeout_s)
{
    auto call = std::make_shared<MainCall>();
    wxTheApp->CallAfter([call]() {
        json titles = json::array();
        for (const auto &t : shown_modal_titles()) titles.push_back(t);
        std::lock_guard<std::mutex> lk(call->m);
        call->result = std::move(titles);
        call->done   = true;
        call->cv.notify_all();
    });
    std::unique_lock<std::mutex> lk(call->m);
    call->cv.wait_for(lk, std::chrono::milliseconds(int(timeout_s * 1000)), [&] { return call->done; });
    std::vector<std::string> out;
    if (call->done)
        for (const auto &t : call->result) out.push_back(t.get<std::string>());
    return out;
}

} // namespace

void set_tool_may_nest(bool may_nest) { t_tool_may_nest = may_nest; }

void post_to_main(std::function<void()> fn)
{
    if (wxTheApp && !g_exiting) wxTheApp->CallAfter(std::move(fn));
}

json run_on_main(std::function<json()> fn, double timeout_s)
{
    if (wxThread::IsMain()) return fn();
    if (g_exiting || !wxTheApp) throw ToolError{"app.exiting", "print_bot is shutting down"};
    auto       call     = std::make_shared<MainCall>();
    const bool may_nest = t_tool_may_nest;
    wxTheApp->CallAfter([call, may_nest, fn = std::move(fn)]() {
        json               r;
        std::exception_ptr ex;
        try {
            if (!may_nest) {
                if (g_main_depth > 0)
                    throw ToolError{"ui.busy", "another agent action is still running in the app (it is waiting in a dialog); "
                                               "answer that dialog (ui_list_dialogs / ui_dialog_click) or wait, then retry"};
                auto modal = shown_modal_titles();
                if (!modal.empty()) {
                    std::string list;
                    for (const auto &d : modal) list += (list.empty() ? "'" : ", '") + d + "'";
                    throw ToolError{"ui.blocked_by_dialog", "the app is waiting for the user in a dialog: " + list +
                                                                ". Answer it first (ui_list_dialogs / ui_dialog_click, or the user); nothing was done."};
                }
                // A context menu runs its own event loop; changing the model under it leaves its handlers with
                // dangling pointers (e.g. undo while a plate's right-click menu is open).
                if (wxCurrentPopupMenu != nullptr)
                    throw ToolError{"ui.busy", "a context menu is open in the app; close it (Escape) and retry. Nothing was done."};
            }
            struct DepthGuard { DepthGuard() { ++g_main_depth; } ~DepthGuard() { --g_main_depth; } } depth_guard;
            r = fn();
        } catch (...) {
            ex = std::current_exception();
        }
        std::lock_guard<std::mutex> lk(call->m);
        call->result = std::move(r);
        call->ex     = ex;
        call->done   = true;
        call->cv.notify_all();
    });
    const auto t0         = std::chrono::steady_clock::now();
    auto       next_probe = t0 + std::chrono::milliseconds(1500);
    std::unique_lock<std::mutex> lk(call->m);
    while (!call->done) {
        call->cv.wait_for(lk, std::chrono::milliseconds(200));
        if (call->done) break;
        const auto now = std::chrono::steady_clock::now();
        if (g_exiting) throw ToolError{"app.exiting", "print_bot is shutting down"};
        if (now >= next_probe) {
            lk.unlock();
            auto dialogs = probe_modal_dialogs(0.5);
            lk.lock();
            if (!call->done && !dialogs.empty()) {
                std::string list;
                for (const auto &d : dialogs) list += (list.empty() ? "'" : ", '") + d + "'";
                throw ToolError{"ui.blocked_by_dialog",
                                "the app is waiting for the user in a dialog: " + list +
                                    ". The action continues once the dialog is answered (ui_list_dialogs / ui_dialog_click, or the user)."};
            }
            next_probe = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        }
        if (std::chrono::duration<double>(now - t0).count() > timeout_s)
            throw ToolError{"ui.timeout", "the app did not finish the action in time (still busy)"};
    }
    if (call->ex) std::rethrow_exception(call->ex);
    return call->result;
}

void agent_activity(const std::string &text)
{
    post_to_main([text]() {
        Plater *plater = wxGetApp().plater();
        if (!plater) return;
        if (NotificationManager *nm = plater->get_notification_manager()) {
            nm->close_notification_of_type(NotificationType::AgentActivity);
            nm->push_notification(NotificationType::AgentActivity, NotificationManager::NotificationLevel::RegularNotificationLevel,
                                  "Agent: " + text);
            plater->get_current_canvas3D()->schedule_extra_frame(0);
        }
    });
}

bool printer_control_allowed()
{
    if (const char *e = std::getenv("PRINTBOT_MCP_ALLOW_PRINTER_CONTROL"); e && std::string(e) == "1") return true;
    AppConfig *cfg = wxGetApp().app_config;
    return cfg && cfg->get("mcp_allow_printer_control") == "true";
}

std::string host_socket_path() { return g_listener && g_listener->running() ? g_listener->path() : std::string(); }

void start_host()
{
    if (g_listener) return;
    if (const char *e = std::getenv("PRINTBOT_MCP_DISABLE"); e && std::string(e) == "1") return;
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg && cfg->get("mcp_server_enabled") == "false") return;

    const std::string path = printbot::mcp::default_socket_path();
    auto             *l    = new printbot::mcp::SocketListener();
    std::string       err;
    bool ok = l->start(path, [](int fd) {
        printbot::mcp::FdInBuf inbuf(fd);
        std::istream           in(&inbuf);
        printbot::mcp::SocketWriter writer;
        if (!writer.open(fd)) return;
        std::FILE *out = writer.file();
        const int n = ++g_clients;
        BOOST_LOG_TRIVIAL(info) << "print_bot MCP: client connected (" << n << " connected)";
        agent_activity("an AI agent connected (MCP)");
        try {
            printbot::mcp::McpServer server(wxGetApp().core(), out, make_gui_tools(), gui_instructions());
            server.run(in);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "print_bot MCP: client session ended by an error: " << e.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "print_bot MCP: client session ended by an unknown error";
        }
        writer.close();
        const int left = --g_clients;
        BOOST_LOG_TRIVIAL(info) << "print_bot MCP: client disconnected (" << left << " connected)";
        if (!g_exiting) agent_activity("the AI agent disconnected");
    }, err);
    if (!ok) {
        BOOST_LOG_TRIVIAL(warning) << "print_bot MCP: live endpoint not started: " << err;
        delete l;
        return;
    }
    g_listener = l;
    BOOST_LOG_TRIVIAL(info) << "print_bot MCP: live endpoint listening on " << path;
}

void stop_host()
{
    g_exiting = true;
    if (g_listener) g_listener->stop();
}

}}} // namespace Slic3r::GUI::pbmcp
