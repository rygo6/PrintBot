// Windows-native E2E probe for bambu_network_update_cert.
//
// tools/plugin_runner is POSIX-only (dlopen + pthread + dl), so this small
// harness covers the same ground for the shared app-cert fetch on Windows:
// LoadLibrary the built plugin, create_agent -> set_config_dir -> start ->
// update_cert against a FRESH data directory, then verify that
// slicer_cert.pem / slicer_crl.pem / slicer_key.pem landed, parse the leaf,
// and print its serial (compare against a known-good config dir manually).
//
// Manual tool: it hits Bambu's live cert endpoint, so it is deliberately NOT
// registered with ctest (network + reachable endpoint required).
//
// Usage:
//   update_cert_probe --plugin <bambu_networking.dll> --data-dir <fresh dir>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

using CreateAgent   = void* (*)(std::string log_dir);
using DestroyAgent  = int (*)(void* agent);
using InitLog       = int (*)(void* agent);
using SetConfigDir  = int (*)(void* agent, std::string config_dir);
using Start         = int (*)(void* agent);
using UpdateCert    = int (*)(void* agent);

struct Plugin {
    HMODULE mod = nullptr;
    CreateAgent  create_agent  = nullptr;
    DestroyAgent destroy_agent = nullptr;
    InitLog      init_log      = nullptr;
    SetConfigDir set_config_dir = nullptr;
    Start        start         = nullptr;
    UpdateCert   update_cert   = nullptr;
    ~Plugin() { if (mod) FreeLibrary(mod); }
};

template <typename T>
bool resolve(Plugin& p, const char* name, T& out)
{
    out = reinterpret_cast<T>(GetProcAddress(p.mod, name));
    if (!out) {
        std::cerr << "FAIL: plugin does not export " << name << "\n";
        return false;
    }
    return true;
}

bool file_nonempty(const fs::path& path)
{
    std::error_code ec;
    return fs::is_regular_file(path, ec) && fs::file_size(path, ec) > 0;
}

// Leaf serial (lowercase hex, even length) of the first PEM certificate.
std::string leaf_serial(const fs::path& cert_path)
{
    std::ifstream f(cert_path, std::ios::binary);
    if (!f) return {};
    std::string pem((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return {};
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert) return {};
    std::string out;
    if (const ASN1_INTEGER* sn = X509_get_serialNumber(cert)) {
        if (BIGNUM* bn = ASN1_INTEGER_to_BN(sn, nullptr)) {
            if (char* hex = BN_bn2hex(bn)) {
                out = hex;
                OPENSSL_free(hex);
                for (char& c : out)
                    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                if (out.size() % 2) out.insert(out.begin(), '0');
            }
            BN_free(bn);
        }
    }
    X509_free(cert);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    std::string plugin_path;
    std::string data_dir;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--plugin")     plugin_path = argv[i + 1];
        else if (a == "--data-dir") data_dir = argv[i + 1];
        else {
            std::cerr << "unknown option: " << a << "\n";
            return 2;
        }
    }
    if (plugin_path.empty() || data_dir.empty()) {
        std::cerr << "usage: update_cert_probe --plugin <dll> --data-dir <fresh dir>\n";
        return 2;
    }

    // Fresh-room proof: an existing credential would make a pass ambiguous.
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    if (fs::exists(fs::path(data_dir) / "slicer_key.pem", ec)) {
        std::cerr << "FAIL: " << data_dir
                  << " already contains slicer_key.pem — use a fresh dir\n";
        return 2;
    }

    Plugin p;
    p.mod = LoadLibraryA(plugin_path.c_str());
    if (!p.mod) {
        std::cerr << "FAIL: LoadLibrary(" << plugin_path
                  << ") error " << GetLastError() << "\n";
        return 1;
    }
    if (!resolve(p, "bambu_network_create_agent", p.create_agent) ||
        !resolve(p, "bambu_network_destroy_agent", p.destroy_agent) ||
        !resolve(p, "bambu_network_init_log", p.init_log) ||
        !resolve(p, "bambu_network_set_config_dir", p.set_config_dir) ||
        !resolve(p, "bambu_network_start", p.start) ||
        !resolve(p, "bambu_network_update_cert", p.update_cert))
        return 1;

    void* agent = p.create_agent(data_dir);
    if (!agent) {
        std::cerr << "FAIL: create_agent returned null (see " << data_dir
                  << "\\obn.log)\n";
        return 1;
    }
    p.init_log(agent);
    if (p.set_config_dir(agent, data_dir) != 0) {
        std::cerr << "FAIL: set_config_dir\n";
        return 1;
    }
    p.start(agent);

    std::cout << "update_cert -> rc=";
    const int rc = p.update_cert(agent);
    std::cout << rc << "\n";

    const fs::path dir(data_dir);
    bool ok = rc == 0;
    for (const char* name : {"slicer_cert.pem", "slicer_crl.pem",
                             "slicer_key.pem"}) {
        const bool present = file_nonempty(dir / name);
        std::cout << "  " << name << ": "
                  << (present ? "present" : "MISSING") << "\n";
        ok = ok && present;
    }
    if (ok) {
        const std::string serial = leaf_serial(dir / "slicer_cert.pem");
        std::cout << "  leaf serial: " << serial << "\n";
        const std::string key_head = [&] {
            std::ifstream f(dir / "slicer_key.pem", std::ios::binary);
            std::string first;
            std::getline(f, first);
            return first;
        }();
        std::cout << "  key header: " << key_head << "\n";
        ok = serial.size() >= 2 &&
             key_head.find("PRIVATE KEY") != std::string::npos;
    }
    p.destroy_agent(agent);
    std::cout << (ok ? "update_cert_probe: PASS\n"
                     : "update_cert_probe: FAIL\n");
    return ok ? 0 : 1;
}