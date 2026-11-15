// print_bot MCP: run a helper program without a shell. GUI-free.
//
// The program gets exactly the given argument vector (no shell, no word splitting, no quoting,
// no globbing), stdin/stdout/stderr on /dev/null, and its own process group so a bounded
// timeout can stop it and anything it started. Nothing about the arguments is logged, so
// arguments that carry credentials (e.g. an authenticated stream URL) stay out of logs.
#ifndef printbot_mcp_Subprocess_hpp_
#define printbot_mcp_Subprocess_hpp_

#include <string>
#include <vector>

namespace printbot::mcp {

struct ProcessResult
{
    bool        started   = false;
    bool        timed_out = false;
    int         exit_code = -1; // valid when the program exited normally
    int         signal    = 0;  // terminating signal, if any
    std::string error;          // why it could not be started (never contains the arguments)
};

// Absolute path of an executable named `name` in $PATH ("" if none). Names containing '/' are
// returned as-is when executable.
std::string find_executable(const std::string &name);

// Runs argv[0] (an absolute path, see find_executable) with argv. Waits at most timeout_s,
// then terminates the process group (SIGTERM, then SIGKILL).
ProcessResult run_process(const std::vector<std::string> &argv, double timeout_s);

// ffmpeg arguments for grabbing one frame from `url` into the file `out_path`. The output is
// passed as "file:<path>" with the image2 muxer in update mode, so ffmpeg treats it as a literal
// file name (no protocol prefix, no %d sequence pattern).
std::vector<std::string> ffmpeg_snapshot_argv(const std::string &ffmpeg, const std::string &url, const std::string &out_path);

} // namespace printbot::mcp

#endif
