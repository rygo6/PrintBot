// ensure_block_cloud_off() + the obn_ensure_conf_block_cloud() export the
// plugin installer calls once OrcaSlicer's audit hard-denies every Python
// open() of a "*.conf" path. The semantics mirror ensure_obn_conf() in
// open_bambu_networking.py: truthy key -> rewrite to 0, no key -> append the
// documented block, key already off -> file untouched, other lines preserved.

#include "obn/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

extern "C" int obn_ensure_conf_block_cloud(const char* conf_path);

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__          \
                      << " " << #cond << "\n";                          \
            return 1;                                                   \
        }                                                               \
    } while (0)

static fs::path make_temp_dir()
{
    const fs::path base = fs::temp_directory_path() / "obn-conf-ensure-test";
    fs::create_directories(base);
    const fs::path dir = base / std::to_string(
        static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

static void write_conf(const fs::path& path, const std::string& body)
{
    std::ofstream out(path, std::ios::binary);
    out << body;
}

static std::string read_conf(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

static int test_missing_file_created()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    CHECK(!fs::exists(path));
    CHECK(obn::config::ensure_block_cloud_off(path.string())
          == obn::config::EnsureOutcome::Created);
    CHECK(fs::exists(path));
    const std::string body = read_conf(path);
    CHECK(body.find("block_cloud = 0") != std::string::npos);
    CHECK(body.find("block_cloud = 1") == std::string::npos);
    // Created from the full template, not a stub.
    CHECK(body.find("cloud_global_api_host = https://api.bambulab.com")
          != std::string::npos);
    return 0;
}

static int test_truthy_flipped_others_preserved()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    write_conf(path,
               "# my conf\n"
               "log_level = debug\n"
               "block_cloud = true\n"
               "lan_tls_skip_verify = 1\n");
    CHECK(obn::config::ensure_block_cloud_off(path.string())
          == obn::config::EnsureOutcome::Set);
    const std::string body = read_conf(path);
    CHECK(body.find("block_cloud = 0\n") != std::string::npos);
    CHECK(body.find("block_cloud = true") == std::string::npos);
    CHECK(body.find("# my conf\n") == 0);
    CHECK(body.find("log_level = debug\n") != std::string::npos);
    CHECK(body.find("lan_tls_skip_verify = 1\n") != std::string::npos);
    return 0;
}

static int test_already_off_untouched()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    write_conf(path, "# header\nblock_cloud = 0\nlog_level = warn\n");
    const std::string before = read_conf(path);
    CHECK(obn::config::ensure_block_cloud_off(path.string())
          == obn::config::EnsureOutcome::Unchanged);
    CHECK(read_conf(path) == before);
    return 0;
}

static int test_false_value_untouched()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    write_conf(path, "block_cloud = false\n");
    const std::string before = read_conf(path);
    CHECK(obn::config::ensure_block_cloud_off(path.string())
          == obn::config::EnsureOutcome::Unchanged);
    CHECK(read_conf(path) == before);
    return 0;
}

static int test_missing_key_appended()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    write_conf(path, "log_level = debug\n");
    CHECK(obn::config::ensure_block_cloud_off(path.string())
          == obn::config::EnsureOutcome::Appended);
    const std::string body = read_conf(path);
    CHECK(body.rfind("log_level = debug\n", 0) == 0);
    CHECK(body.find("block_cloud = 0\n") != std::string::npos);
    CHECK(body.find("talk to Bambu Cloud") != std::string::npos);
    return 0;
}

static int test_comment_is_not_the_key()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    write_conf(path, "# block_cloud = 1\nlog_level = debug\n");
    CHECK(obn::config::ensure_block_cloud_off(path.string())
          == obn::config::EnsureOutcome::Appended);
    const std::string body = read_conf(path);
    CHECK(body.find("# block_cloud = 1\n") == 0);
    CHECK(body.find("\nblock_cloud = 0\n") != std::string::npos);
    return 0;
}

static int test_missing_final_newline_gains_one()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    write_conf(path, "block_cloud = 1");
    CHECK(obn::config::ensure_block_cloud_off(path.string())
          == obn::config::EnsureOutcome::Set);
    const std::string body = read_conf(path);
    CHECK(body == "block_cloud = 0\n");
    return 0;
}

static int test_crlf_endings_preserved()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    write_conf(path, "log_level = debug\r\nblock_cloud = 1\r\n");
    CHECK(obn::config::ensure_block_cloud_off(path.string())
          == obn::config::EnsureOutcome::Set);
    const std::string body = read_conf(path);
    CHECK(body.find("log_level = debug\r\n") != std::string::npos);
    CHECK(body.find("block_cloud = 0\r\n") != std::string::npos);
    CHECK(body.find("block_cloud = 1") == std::string::npos);
    return 0;
}

static int test_existing_empty_file_appended()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    write_conf(path, "");
    CHECK(obn::config::ensure_block_cloud_off(path.string())
          == obn::config::EnsureOutcome::Appended);
    CHECK(read_conf(path).find("block_cloud = 0\n") != std::string::npos);
    return 0;
}

static int test_empty_path_is_error()
{
    CHECK(obn::config::ensure_block_cloud_off("")
          == obn::config::EnsureOutcome::Error);
    CHECK(obn_ensure_conf_block_cloud(nullptr) == -1);
    CHECK(obn_ensure_conf_block_cloud("") == -1);
    return 0;
}

static int test_abi_export_roundtrip()
{
    const fs::path dir  = make_temp_dir();
    const fs::path path = dir / obn::config::kConfigFileName;
    write_conf(path, "block_cloud = 1\n");
    // 2 = Set, the same enum value the Python installer maps to its wording.
    CHECK(obn_ensure_conf_block_cloud(path.string().c_str()) == 2);
    CHECK(read_conf(path).find("block_cloud = 0\n") != std::string::npos);
    return 0;
}

int main()
{
    if (test_missing_file_created() != 0) return 1;
    if (test_truthy_flipped_others_preserved() != 0) return 1;
    if (test_already_off_untouched() != 0) return 1;
    if (test_false_value_untouched() != 0) return 1;
    if (test_missing_key_appended() != 0) return 1;
    if (test_comment_is_not_the_key() != 0) return 1;
    if (test_missing_final_newline_gains_one() != 0) return 1;
    if (test_crlf_endings_preserved() != 0) return 1;
    if (test_existing_empty_file_appended() != 0) return 1;
    if (test_empty_path_is_error() != 0) return 1;
    if (test_abi_export_roundtrip() != 0) return 1;

    std::cout << "conf_ensure_test: ok\n";
    return 0;
}
