// print_bot-mcp: MCP server for print_bot (stdio transport).
//
//   print_bot-mcp [--headless | --gui] [--socket PATH] [--datadir DIR] [--resources DIR] [--tempdir DIR] [--log-level 0-5]
//
// Live mode (default when the print_bot app is running): bridges stdio to the app's MCP socket,
// so every tool call acts on the running app and shows up in its window. Headless mode (no app
// running, or --headless): serves a GUI-free core in this process; no display needed.
//
// stdout carries only MCP JSON-RPC messages; everything else (logs, stray prints from the
// slicing engine) goes to stderr.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/nowide/args.hpp>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "libslic3r/Utils.hpp"

#include "core/Core.hpp"
#include "core/Version.hpp"
#include "McpServer.hpp"
#include "McpSocket.hpp"

namespace fs = boost::filesystem;
using namespace printbot;

namespace {

// Keep the protocol channel clean: before any other static initializer runs (the engine logs
// from static init), duplicate the real stdout for MCP messages and point fd 1 at stderr, so
// any stray printf/cout/log output lands in the log instead of the JSON-RPC stream.
struct ProtocolStdout
{
    int fd = -1;
    ProtocolStdout()
    {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        fd = _dup(1);
        _dup2(2, 1);
#else
        fd = dup(1);
        dup2(2, 1);
#endif
    }
};
#if defined(__GNUC__) && !defined(_WIN32)
__attribute__((init_priority(101)))
#endif
ProtocolStdout g_protocol_stdout;

std::FILE *protocol_stream()
{
#ifdef _WIN32
    static std::FILE *f = _fdopen(g_protocol_stdout.fd, "wb");
#else
    static std::FILE *f = fdopen(g_protocol_stdout.fd, "w");
#endif
    return f;
}

void usage()
{
    std::fprintf(stderr,
                 "print_bot-mcp %s - MCP server (stdio) for print_bot\n"
                 "usage: print_bot-mcp [--headless | --gui] [--socket PATH] [--datadir DIR] [--resources DIR] [--tempdir DIR] [--log-level N]\n"
                 "  (default)    if the print_bot app is running, drive it live (its window shows every change);\n"
                 "               otherwise run headless\n"
                 "  --gui        require the running app (wait up to --wait seconds for it, default 0; fail if absent)\n"
                 "  --headless   never connect to the app; slice in this process\n"
                 "  --socket     app MCP socket (default: $PRINTBOT_MCP_SOCKET, else $XDG_RUNTIME_DIR/print_bot/mcp.sock)\n"
                 "  --wait       with --gui: seconds to wait for the app to come up\n"
                 "headless options:\n"
                 "  --datadir    settings/presets dir (default: $PRINTBOT_DATADIR, else $XDG_CONFIG_HOME/PrintBot,\n"
                 "               else ~/.config/PrintBot - the same presets the GUI uses; only read)\n"
                 "  --resources  resources dir with profiles/ (default: next to the binary)\n"
                 "  --tempdir    scratch dir for temporary G-code (default: system temp)\n"
                 "  --log-level  0 fatal .. 5 trace, logged to stderr (default 1)\n",
                 PRINTBOT_CORE_VERSION);
}

std::string default_datadir()
{
    if (const char *d = std::getenv("PRINTBOT_DATADIR"); d && *d) return d;
#ifdef _WIN32
    if (const char *a = std::getenv("APPDATA"); a && *a) return (fs::path(a) / "PrintBot").string();
#elif defined(__APPLE__)
    if (const char *h = std::getenv("HOME"); h && *h) return (fs::path(h) / "Library/Application Support/PrintBot").string();
#else
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x) return (fs::path(x) / "PrintBot").string();
    if (const char *h = std::getenv("HOME"); h && *h) return (fs::path(h) / ".config/PrintBot").string();
#endif
    return "PrintBot";
}

std::string default_resources(const char *argv0)
{
    boost::system::error_code ec;
    fs::path exe = fs::canonical(fs::path(argv0), ec);
#ifdef __linux__
    if (ec || exe.empty()) exe = fs::read_symlink("/proc/self/exe", ec);
#endif
    const fs::path dir = exe.parent_path();
    for (const fs::path &cand : {dir / "../resources", dir / "resources", dir / "../../resources", dir / "../Resources"})
        if (fs::exists(cand / "profiles")) return fs::canonical(cand).string();
    return (dir / "../resources").string();
}

} // namespace

int main(int argc, char **argv)
{
    boost::nowide::args nargs(argc, argv);

    core::CoreOptions opts;
    opts.mode      = core::CoreOptions::Mode::Headless;
    int log_level  = 1;
    enum class Want { Auto, Gui, Headless } want = Want::Auto;
    std::string socket_path;
    double      wait_s = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a    = argv[i];
        auto        next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--headless") want = Want::Headless;
        else if (a == "--gui") want = Want::Gui;
        else if (a == "--socket") socket_path = next();
        else if (a == "--wait") wait_s = std::atof(next().c_str());
        else if (a == "--datadir") opts.data_dir = next();
        else if (a == "--resources") opts.resources_dir = next();
        else if (a == "--tempdir") opts.temp_dir = next();
        else if (a == "--log-level") log_level = std::atoi(next().c_str());
        else if (a == "--version") {
            std::fprintf(protocol_stream(), "print_bot-mcp %s (slicer %s)\n", PRINTBOT_CORE_VERSION, core::Core::slicer_version().c_str());
            return 0;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    std::FILE *proto = protocol_stream();
    if (!proto) {
        std::fprintf(stderr, "print_bot-mcp: cannot open protocol stream\n");
        return 1;
    }

    // Live mode: bridge to the running app.
    if (want != Want::Headless) {
        if (socket_path.empty()) socket_path = mcp::default_socket_path();
        int sock = mcp::connect_socket(socket_path);
        for (int i = 0; sock < 0 && want == Want::Gui && i < int(wait_s * 4); ++i) {
#ifndef _WIN32
            usleep(250000);
#endif
            sock = mcp::connect_socket(socket_path);
        }
        if (sock >= 0) {
            std::fprintf(stderr, "print_bot-mcp %s: live mode, bridging to the running print_bot app (%s)\n", PRINTBOT_CORE_VERSION, socket_path.c_str());
            std::fflush(proto);
            int rc = mcp::bridge_stdio(sock, 0, fileno(proto));
#ifndef _WIN32
            close(sock);
#endif
            return rc;
        }
        if (want == Want::Gui) {
            std::fprintf(stderr, "print_bot-mcp: --gui given but the print_bot app is not running (no MCP socket at %s)\n", socket_path.c_str());
            return 1;
        }
        std::fprintf(stderr, "print_bot-mcp: print_bot app not running (%s); starting headless\n", socket_path.c_str());
    }

    if (opts.data_dir.empty()) opts.data_dir = default_datadir();
    if (opts.resources_dir.empty()) opts.resources_dir = default_resources(argv[0]);

    Slic3r::set_logging_level(unsigned(std::max(0, std::min(5, log_level))));

    auto created = core::Core::create(opts);
    if (!created) {
        std::fprintf(stderr, "print_bot-mcp: core init failed: %s: %s\n", created.error().code.c_str(), created.error().message.c_str());
        return 1;
    }
    std::unique_ptr<core::Core> core = std::move(created.value());
    for (const auto &w : core->startup_warnings()) std::fprintf(stderr, "print_bot-mcp: %s\n", w.c_str());
    std::fprintf(stderr, "print_bot-mcp %s ready (datadir %s)\n", PRINTBOT_CORE_VERSION, opts.data_dir.c_str());

    int rc;
    {
        mcp::McpServer server(*core, proto, mcp::make_tools(), mcp::headless_instructions());
        rc = server.run(std::cin);
    }
    core.reset();
    std::fclose(proto);
    return rc;
}
