#include "Subprocess.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace printbot::mcp {

std::vector<std::string> ffmpeg_snapshot_argv(const std::string &ffmpeg, const std::string &url, const std::string &out_path)
{
    return {ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "quiet", "-y", "-rtsp_transport", "tcp", "-i", url,
            "-frames:v", "1", "-f", "image2", "-update", "1", "file:" + out_path};
}

#ifdef _WIN32

std::string find_executable(const std::string &) { return {}; }

ProcessResult run_process(const std::vector<std::string> &, double)
{
    ProcessResult r;
    r.error = "running helper programs is not supported on Windows yet";
    return r;
}

#else

namespace {
bool is_executable(const std::string &p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p.c_str(), X_OK) == 0;
}
} // namespace

std::string find_executable(const std::string &name)
{
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos) return is_executable(name) ? name : std::string();
    const char *path = std::getenv("PATH");
    std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t      pos  = 0;
    while (pos <= dirs.size()) {
        size_t      end = dirs.find(':', pos);
        std::string dir = dirs.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        // Only absolute PATH entries: an empty or relative entry would mean the current directory.
        if (!dir.empty() && dir[0] == '/') {
            const std::string cand = dir + "/" + name;
            if (is_executable(cand)) return cand;
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}

ProcessResult run_process(const std::vector<std::string> &argv, double timeout_s)
{
    ProcessResult r;
    if (argv.empty() || argv[0].empty() || argv[0][0] != '/') {
        r.error = "the program path must be absolute";
        return r;
    }
    std::vector<char *> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int sig : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD, SIGUSR1, SIGUSR2}) sigaddset(&defaults, sig);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0); // own process group, so the timeout can stop its children too
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t     pid = -1;
    const int rc  = posix_spawn(&pid, cargv[0], &fa, &attr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        r.error = std::string("could not start the program: ") + std::strerror(rc);
        return r;
    }
    r.started = true;

    auto       reap     = [&](int options) -> bool {
        int   status = 0;
        pid_t w;
        do {
            w = ::waitpid(pid, &status, options);
        } while (w < 0 && errno == EINTR);
        if (w == pid) {
            if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) r.signal = WTERMSIG(status);
            return true;
        }
        return w < 0; // ECHILD: someone else reaped it; nothing left to wait for
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(timeout_s * 1000));
    while (!reap(WNOHANG)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            r.timed_out = true;
            ::kill(-pid, SIGTERM);
            const auto grace = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!reap(WNOHANG)) {
                if (std::chrono::steady_clock::now() >= grace) {
                    ::kill(-pid, SIGKILL);
                    reap(0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return r;
}

#endif

} // namespace printbot::mcp
