#include "McpSocket.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace printbot::mcp {

std::string default_socket_path()
{
    if (const char *p = std::getenv("PRINTBOT_MCP_SOCKET"); p && *p) return p;
#ifdef _WIN32
    return {};
#else
    if (const char *x = std::getenv("XDG_RUNTIME_DIR"); x && *x) return std::string(x) + "/print_bot/mcp.sock";
    const char *t = std::getenv("TMPDIR");
    return std::string(t && *t ? t : "/tmp") + "/print_bot-" + std::to_string(getuid()) + "/mcp.sock";
#endif
}

#ifdef _WIN32

int connect_socket(const std::string &) { return -1; }
int bridge_stdio(int, int, int) { return 1; }
FdInBuf::int_type FdInBuf::underflow() { return traits_type::eof(); }
SocketListener::~SocketListener() {}
bool SocketListener::start(const std::string &, Handler, std::string &error)
{
    error = "the MCP socket is not supported on Windows yet";
    return false;
}
void SocketListener::stop() {}
void SocketListener::accept_loop() {}

#else

namespace {

bool fill_addr(const std::string &path, sockaddr_un &addr)
{
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

bool write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        len -= size_t(n);
    }
    return true;
}

} // namespace

int connect_socket(const std::string &path)
{
    sockaddr_un addr;
    if (!fill_addr(path, addr)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int bridge_stdio(int sock, int in_fd, int out_fd)
{
    char buf[16384];
    bool in_open = true;
    while (true) {
        pollfd fds[2] = {{sock, POLLIN, 0}, {in_fd, POLLIN, 0}};
        int    n      = ::poll(fds, in_open ? 2 : 1, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = ::read(sock, buf, sizeof(buf));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return 0; // app closed the connection (or exited)
            if (!write_all(out_fd, buf, size_t(r))) return 1;
        }
        if (in_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t r = ::read(in_fd, buf, sizeof(buf));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {
                // Client closed stdin: tell the app; keep relaying until it has answered in-flight calls.
                in_open = false;
                ::shutdown(sock, SHUT_WR);
            } else if (!write_all(sock, buf, size_t(r))) {
                return 1;
            }
        }
    }
}

FdInBuf::int_type FdInBuf::underflow()
{
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
    ssize_t n;
    do {
        n = ::read(m_fd, m_buf, sizeof(m_buf));
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return traits_type::eof();
    setg(m_buf, m_buf, m_buf + n);
    return traits_type::to_int_type(*gptr());
}

SocketListener::~SocketListener() { stop(); }

bool SocketListener::start(const std::string &path, Handler handler, std::string &error)
{
    sockaddr_un addr;
    if (!fill_addr(path, addr)) {
        error = "socket path is empty or too long: " + path;
        return false;
    }
    // Private parent dir (0700).
    const auto slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string dir = path.substr(0, slash);
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            error = "cannot create " + dir + ": " + std::strerror(errno);
            return false;
        }
        struct stat st;
        if (::stat(dir.c_str(), &st) == 0 && st.st_uid != ::getuid()) {
            error = dir + " is owned by another user";
            return false;
        }
        ::chmod(dir.c_str(), 0700);
    }
    // Another live instance? Then leave its socket alone.
    if (int fd = connect_socket(path); fd >= 0) {
        ::close(fd);
        error = "another print_bot instance already serves MCP on " + path;
        return false;
    }
    ::unlink(path.c_str()); // stale socket from a crashed instance

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("socket(): ") + std::strerror(errno);
        return false;
    }
    const mode_t old_mask = ::umask(0177); // socket file 0600
    const int    rc       = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    ::umask(old_mask);
    if (rc != 0 || ::listen(fd, 8) != 0) {
        error = "bind/listen " + path + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::chmod(path.c_str(), 0600);
    m_path      = path;
    m_handler   = std::move(handler);
    m_stop      = false;
    m_listen_fd = fd;
    m_accept_thread = std::thread([this]() { accept_loop(); });
    return true;
}

void SocketListener::accept_loop()
{
    while (!m_stop) {
        pollfd p{m_listen_fd, POLLIN, 0};
        int    n = ::poll(&p, 1, 250);
        if (n <= 0) continue;
        int c = ::accept4(m_listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) continue;
        // Only the same user may talk to the app.
        struct ucred cred;
        socklen_t    len = sizeof(cred);
        if (::getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != ::getuid()) {
            ::close(c);
            continue;
        }
        try {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_conn_fds.insert(c);
            m_conn_threads.emplace_back([this, c]() {
                // Connection boundary: a failing client must never take the app down.
                try {
                    m_handler(c);
                } catch (const std::exception &e) {
                    std::fprintf(stderr, "print_bot MCP: connection ended by an error: %s\n", e.what());
                } catch (...) {
                    std::fprintf(stderr, "print_bot MCP: connection ended by an unknown error\n");
                }
                {
                    std::lock_guard<std::mutex> lk2(m_mutex);
                    m_conn_fds.erase(c);
                }
                ::close(c);
            });
        } catch (...) {
            // e.g. std::system_error when no thread can be created: drop this client, keep accepting.
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_conn_fds.erase(c);
            }
            ::close(c);
        }
    }
}

void SocketListener::stop()
{
    if (m_listen_fd < 0) return;
    m_stop = true;
    if (m_accept_thread.joinable()) m_accept_thread.join();
    ::close(m_listen_fd);
    m_listen_fd = -1;
    ::unlink(m_path.c_str());
    std::list<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (int c : m_conn_fds) ::shutdown(c, SHUT_RDWR);
        threads.swap(m_conn_threads);
    }
    // Connection threads may be waiting on the (exiting) UI thread; don't block shutdown on them.
    for (auto &t : threads)
        if (t.joinable()) t.detach();
}

#endif

} // namespace printbot::mcp
