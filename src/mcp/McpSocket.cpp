#include "McpSocket.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <algorithm>
#include <vector>
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <sddl.h>
#include <fcntl.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#else
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
    if (const char *l = std::getenv("LOCALAPPDATA"); l && *l) return std::string(l) + "\\PrintBot\\mcp.sock";
    return {};
#else
    if (const char *x = std::getenv("XDG_RUNTIME_DIR"); x && *x) return std::string(x) + "/print_bot/mcp.sock";
    const char *t = std::getenv("TMPDIR");
    return std::string(t && *t ? t : "/tmp") + "/print_bot-" + std::to_string(getuid()) + "/mcp.sock";
#endif
}

#ifdef _WIN32

// Windows: AF_UNIX stream sockets through Winsock (Windows 10 1803+). The socket file lives in
// %LOCALAPPDATA%\PrintBot, which only the user (plus SYSTEM/Administrators) can reach, and
// every accepted peer's process must run as the same user (checked via SIO_AF_UNIX_GETPEERPID).
namespace {

bool winsock_ready()
{
    static const bool ok = []() {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
}

std::wstring widen(const std::string &s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

bool fill_addr(const std::string &path, sockaddr_un &addr)
{
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

SOCKET to_sock(int fd) { return SOCKET(intptr_t(fd)); }

bool send_all(SOCKET s, const char *data, size_t len)
{
    while (len > 0) {
        const int n = ::send(s, data, int(std::min<size_t>(len, 1 << 20)), 0);
        if (n == SOCKET_ERROR) return false;
        data += n;
        len -= size_t(n);
    }
    return true;
}

bool write_all_fd(int fd, const char *data, size_t len)
{
    while (len > 0) {
        const int n = _write(fd, data, unsigned(std::min<size_t>(len, 1 << 20)));
        if (n < 0) return false;
        data += n;
        len -= size_t(n);
    }
    return true;
}

// The user SID of a process token, as a string ("" on failure).
std::string token_user_sid(HANDLE token)
{
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    if (len == 0) return {};
    std::vector<unsigned char> buf(len);
    if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len)) return {};
    LPSTR str = nullptr;
    if (!ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &str)) return {};
    std::string out(str);
    LocalFree(str);
    return out;
}

std::string process_user_sid(DWORD pid)
{
    HANDLE proc = pid == GetCurrentProcessId() ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    std::string sid;
    HANDLE      token = nullptr;
    if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        sid = token_user_sid(token);
        CloseHandle(token);
    }
    if (proc != GetCurrentProcess()) CloseHandle(proc);
    return sid;
}

bool peer_is_same_user(SOCKET c)
{
    ULONG pid = 0;
    DWORD got = 0;
    if (WSAIoctl(c, SIO_AF_UNIX_GETPEERPID, nullptr, 0, &pid, sizeof(pid), &got, nullptr, nullptr) != 0 || pid == 0) return false;
    static const std::string me = process_user_sid(GetCurrentProcessId());
    return !me.empty() && process_user_sid(pid) == me;
}

} // namespace

int connect_socket(const std::string &path)
{
    sockaddr_un addr;
    if (!winsock_ready() || !fill_addr(path, addr)) return -1;
    SOCKET s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    if (::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::closesocket(s);
        return -1;
    }
    return int(intptr_t(s));
}

void close_socket(int fd)
{
    if (fd >= 0) ::closesocket(to_sock(fd));
}

int bridge_stdio(int sock, int in_fd, int out_fd)
{
    const SOCKET s = to_sock(sock);
    // stdin can't be polled together with a socket on Windows: pump it on its own thread.
    std::thread([s, in_fd]() {
        char buf[16384];
        while (true) {
            const int r = _read(in_fd, buf, sizeof(buf));
            if (r <= 0) {
                // Client closed stdin: tell the app; keep relaying until it has answered in-flight calls.
                ::shutdown(s, SD_SEND);
                return;
            }
            if (!send_all(s, buf, size_t(r))) return;
        }
    }).detach();
    char buf[16384];
    while (true) {
        const int r = ::recv(s, buf, sizeof(buf), 0);
        if (r == 0) return 0; // app closed the connection (or exited)
        if (r < 0) return 1;
        if (!write_all_fd(out_fd, buf, size_t(r))) return 1;
    }
}

FdInBuf::int_type FdInBuf::underflow()
{
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
    const int n = ::recv(to_sock(m_fd), m_buf, sizeof(m_buf), 0);
    if (n <= 0) return traits_type::eof();
    setg(m_buf, m_buf, m_buf + n);
    return traits_type::to_int_type(*gptr());
}

bool SocketWriter::open(int sock)
{
    // A CRT FILE* can't wrap a socket on Windows: write into an anonymous pipe and pump it.
    int fds[2];
    if (_pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) != 0) return false;
    m_file = _fdopen(fds[1], "wb");
    if (!m_file) {
        _close(fds[0]);
        _close(fds[1]);
        return false;
    }
    const int rd = fds[0];
    m_pump       = std::thread([rd, sock]() {
        char buf[16384];
        bool ok = true;
        while (true) {
            const int r = _read(rd, buf, sizeof(buf));
            if (r <= 0) break;
            if (ok) ok = send_all(to_sock(sock), buf, size_t(r)); // keep draining so writers never block
        }
        _close(rd);
    });
    return true;
}

void SocketWriter::close()
{
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
    if (m_pump.joinable()) m_pump.join();
}

SocketListener::~SocketListener() { stop(); }

bool SocketListener::start(const std::string &path, Handler handler, std::string &error)
{
    sockaddr_un addr;
    if (!winsock_ready()) {
        error = "Winsock initialization failed";
        return false;
    }
    if (!fill_addr(path, addr)) {
        error = "socket path is empty or too long: " + path;
        return false;
    }
    const auto slash = path.find_last_of("\\/");
    if (slash != std::string::npos && slash > 0) {
        const std::wstring dir = widen(path.substr(0, slash));
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            error = "cannot create " + path.substr(0, slash);
            return false;
        }
    }
    // Another live instance? Then leave its socket alone.
    if (int fd = connect_socket(path); fd >= 0) {
        close_socket(fd);
        error = "another print_bot instance already serves MCP on " + path;
        return false;
    }
    DeleteFileW(widen(path).c_str()); // stale socket from a crashed instance

    SOCKET s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        error = "socket(): WSA error " + std::to_string(WSAGetLastError());
        return false;
    }
    if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(s, 8) != 0) {
        error = "bind/listen " + path + ": WSA error " + std::to_string(WSAGetLastError());
        ::closesocket(s);
        return false;
    }
    m_path          = path;
    m_handler       = std::move(handler);
    m_stop          = false;
    m_listen_fd     = int(intptr_t(s));
    m_accept_thread = std::thread([this]() { accept_loop(); });
    return true;
}

void SocketListener::accept_loop()
{
    while (!m_stop) {
        WSAPOLLFD p{to_sock(m_listen_fd), POLLRDNORM, 0};
        if (WSAPoll(&p, 1, 250) <= 0) continue;
        SOCKET cs = ::accept(to_sock(m_listen_fd), nullptr, nullptr);
        if (cs == INVALID_SOCKET) continue;
        // Only the same user may talk to the app.
        if (!peer_is_same_user(cs)) {
            ::closesocket(cs);
            continue;
        }
        const int c = int(intptr_t(cs));
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
                ::closesocket(to_sock(c));
            });
        } catch (...) {
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_conn_fds.erase(c);
            }
            ::closesocket(cs);
        }
    }
}

void SocketListener::stop()
{
    if (m_listen_fd < 0) return;
    m_stop = true;
    if (m_accept_thread.joinable()) m_accept_thread.join();
    ::closesocket(to_sock(m_listen_fd));
    m_listen_fd = -1;
    DeleteFileW(widen(m_path).c_str());
    std::list<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (int c : m_conn_fds) ::shutdown(to_sock(c), SD_BOTH);
        threads.swap(m_conn_threads);
    }
    for (auto &t : threads)
        if (t.joinable()) t.detach();
}

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

// AF_UNIX stream socket with close-on-exec (SOCK_CLOEXEC is Linux/BSD only; macOS needs fcntl).
int new_unix_socket()
{
#ifdef SOCK_CLOEXEC
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
#ifdef SO_NOSIGPIPE
    // macOS has no MSG_NOSIGNAL: a write to a closed peer must not kill the process.
    if (fd >= 0) { int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
    return fd;
}

int accept_cloexec(int listen_fd)
{
#ifdef __linux__
    return ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
    int c = ::accept(listen_fd, nullptr, nullptr);
    if (c >= 0) {
        ::fcntl(c, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    }
    return c;
#endif
}

// True if the connected peer runs as the same user as this process.
bool peer_is_same_user(int c)
{
#ifdef __linux__
    struct ucred cred;
    socklen_t    len = sizeof(cred);
    return ::getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == ::getuid();
#else
    uid_t uid = 0;
    gid_t gid = 0;
    return ::getpeereid(c, &uid, &gid) == 0 && uid == ::getuid();
#endif
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
    int fd = new_unix_socket();
    if (fd < 0) return -1;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

void close_socket(int fd)
{
    if (fd >= 0) ::close(fd);
}

bool SocketWriter::open(int sock)
{
    const int d = ::dup(sock);
    if (d < 0) return false;
    m_file = fdopen(d, "w");
    if (!m_file) ::close(d);
    return m_file != nullptr;
}

void SocketWriter::close()
{
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
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

    int fd = new_unix_socket();
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
        int c = accept_cloexec(m_listen_fd);
        if (c < 0) continue;
        // Only the same user may talk to the app.
        if (!peer_is_same_user(c)) {
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
