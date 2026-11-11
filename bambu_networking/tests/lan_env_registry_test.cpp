// LAN env registry hydration: obn.env persists the ip<->serial and ip<->peer
// pairs that let a fresh process mint a LAN camera URL before the next SSDP
// NOTIFY arrives. Two regressions are pinned here:
//   1. hydrate only mirrored the pairs into process env, never into the
//      g_ip_to_serial / g_ip_to_peer_cert maps, so registry_lookup_serial /
//      registry_ip_for_serial missed them cross-process;
//   2. write_state_file_locked rebuilds obn.env from the maps, so the first
//      write in a fresh process (empty map) erased the persisted pairs.
#include "obn/lan_tls.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fs = std::filesystem;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__          \
                      << " " << #cond << "\n";                          \
            return 1;                                                   \
        }                                                               \
    } while (0)

static void set_env(const std::string& key, const std::string& val)
{
#if defined(_WIN32)
    ::SetEnvironmentVariableA(key.c_str(), val.c_str());
    (void)_putenv_s(key.c_str(), val.c_str());
#else
    (void)::setenv(key.c_str(), val.c_str(), 1);
#endif
}

static fs::path make_temp_dir()
{
    const fs::path base = fs::temp_directory_path() / "obn-lan-env-registry";
    fs::create_directories(base);
    const fs::path dir = base / std::to_string(
        static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

static std::string read_all(const fs::path& path)
{
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

static int test_hydrate_and_no_clobber()
{
    const fs::path dir = make_temp_dir();
    {
        std::ofstream out(dir / "obn.env");
        out << "# Open Bamboo Networking LAN TLS IPC (auto-generated)\n"
               "OBN_LAN_TLS_CA_FILE=/x/printer.cer\n"
               "OBN_LAN_TLS_IP_192_168_2_110=01P00C490800259\n"
               "OBN_LAN_TLS_PEER_192_168_2_110=C:\\certs\\01.pem\n"
               "OBN_LAN_TLS_IP_vpn_gateway=BADSERIAL\n"
               "OBN_SKIP_TLS_VERIFY=1\n";
    }
    // state_file_search_paths() prefers the snapshot config dir, then
    // OBN_CONFIG_DIR; the snapshot is empty until registry_set_config_dir,
    // so the env var steers this (and only this) test process at the temp
    // file. Must happen before the first lan_tls call hydrates.
    set_env("OBN_CONFIG_DIR", dir.string());

    // Reverse and forward lookups both hydrate from the file on first use.
    CHECK(obn::lan_tls::registry_ip_for_serial("01P00C490800259")
          == "192.168.2.110");
    const auto serial =
        obn::lan_tls::registry_lookup_serial("192.168.2.110");
    CHECK(serial.has_value());
    CHECK(*serial == "01P00C490800259");
    // Malformed keys (non-IPv4 after the '_'->'.' decode) are not pairs.
    CHECK(obn::lan_tls::registry_ip_for_serial("BADSERIAL").empty());
    CHECK(obn::lan_tls::registry_ip_for_serial("NOSUCH").empty());

    // The first write in a fresh process must not erase the persisted pairs
    // (empty-map rewrite regression): set_config_dir syncs the maps back.
    obn::lan_tls::registry_set_config_dir(dir.string());
    std::string content = read_all(dir / "obn.env");
    CHECK(content.find("OBN_LAN_TLS_IP_192_168_2_110=01P00C490800259")
          != std::string::npos);
    CHECK(content.find("OBN_LAN_TLS_PEER_192_168_2_110=") != std::string::npos);

    // A new pair merges alongside the hydrated ones and both survive.
    obn::lan_tls::registry_put_ip_serial("192.168.2.77", "02NEWSERIAL01");
    content = read_all(dir / "obn.env");
    CHECK(content.find("OBN_LAN_TLS_IP_192_168_2_110=01P00C490800259")
          != std::string::npos);
    CHECK(content.find("OBN_LAN_TLS_IP_192_168_2_77=02NEWSERIAL01")
          != std::string::npos);
    CHECK(obn::lan_tls::registry_ip_for_serial("02NEWSERIAL01")
          == "192.168.2.77");

    std::error_code ec;
    fs::remove_all(dir, ec);
    return 0;
}

int main()
{
    return test_hydrate_and_no_clobber();
}
