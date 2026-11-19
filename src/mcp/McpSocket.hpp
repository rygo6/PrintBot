// print_bot MCP: local socket transport. The running print_bot app listens on a Unix domain
// socket (only the user can connect: the socket dir is 0700 and the socket 0600), and
// print_bot-mcp bridges an MCP client's stdio to it, so tool calls act on the live app and
// the user sees every change. GUI-free (no wxWidgets). On Windows the socket is a Winsock AF_UNIX socket in
// %LOCALAPPDATA%\\PrintBot, and peers must run as the same user.
#ifndef printbot_mcp_McpSocket_hpp_
#define printbot_mcp_McpSocket_hpp_

#include <atomic>
#include <cstdio>
#include <functional>
#include <list>
#include <mutex>
#include <set>
#include <streambuf>
#include <string>
#include <thread>

namespace printbot::mcp {

// $PRINTBOT_MCP_SOCKET, else $XDG_RUNTIME_DIR/print_bot/mcp.sock, else <tmp>/print_bot-<uid>/mcp.sock
// (Windows: %LOCALAPPDATA%\\PrintBot\\mcp.sock).
std::string default_socket_path();

// Connects to a listening socket. Returns the fd, or -1 (errno set) if nobody listens there.
int connect_socket(const std::string &path);

// Closes a socket returned by connect_socket().
void close_socket(int fd);

// Relays bytes between an MCP client (in_fd/out_fd, e.g. stdin/stdout) and a connected socket
// until the socket closes. Returns 0 on a clean end.
int bridge_stdio(int sock, int in_fd, int out_fd);

// std::streambuf reading from a file descriptor (used to feed McpServer::run()).
class FdInBuf : public std::streambuf
{
public:
    explicit FdInBuf(int fd) : m_fd(fd) {}

protected:
    int_type underflow() override;

private:
    int  m_fd;
    char m_buf[16384];
};

// A stdio FILE* whose writes go to a connected socket (POSIX: fdopen of a dup; Windows: a CRT pipe
// pumped into the socket by a thread). close() flushes and waits until everything is sent.
class SocketWriter
{
public:
    ~SocketWriter() { close(); }
    bool       open(int sock);
    void       close();
    std::FILE *file() const { return m_file; }

private:
    std::FILE  *m_file = nullptr;
    std::thread m_pump;
};

class SocketListener
{
public:
    using Handler = std::function<void(int fd)>; // runs on its own thread; must not close fd

    ~SocketListener();
    // Binds and starts accepting. Fails (returns false + message) when another live process
    // already listens on the path; a stale socket file is replaced.
    bool start(const std::string &path, Handler handler, std::string &error);
    void stop();
    const std::string &path() const { return m_path; }
    bool               running() const { return m_listen_fd >= 0; }

private:
    void accept_loop();

    std::string            m_path;
    Handler                m_handler;
    std::atomic<int>       m_listen_fd{-1};
    std::atomic<bool>      m_stop{false};
    std::thread            m_accept_thread;
    std::mutex             m_mutex;
    std::set<int>          m_conn_fds;
    std::list<std::thread> m_conn_threads;
};

} // namespace printbot::mcp

#endif
