// EXP-01 — opt-in `exp_numeric_sequence_id` obn.conf flag (default off).
//
// Locks the three properties the flag must hold on the four self-built signed
// frames (print_job.cpp project_file; agent.cpp rescue_cloud_project_file,
// rescue_cloud_liveview, liveview prepare):
//   (a)(b) flag OFF  -> byte-identical legacy string form, both before any
//                       config load and with a loaded conf that LACKS the key;
//   (c)    flag ON   -> bare JSON number through parse -> dump;
//   (d)    flag ON   -> the number survives obn::signing::maybe_sign;
//   (e)    flag ON   -> out-of-scope frames keep their string form because
//                       next_mqtt_seq_id() still returns the 20000-29999 digit
//                       string that the security.* / pushall embedders quote
//                       with their own literal quotes.
//
// Framework: repo convention (int main() + CHECK, first failure returns 1)
//
// No sockets, no runtime network: every input here is a fixed string.

#include "obn/config.hpp"
#include "obn/json_lite.hpp"
#include "obn/mqtt_seq.hpp"
#include "obn/signing.hpp"

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

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
    const fs::path base = fs::temp_directory_path() / "obn-exp-seq-test";
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

// Print-shaped frame (root key "print", signable) assembled with the
// flag-aware Value twin, then parse -> dump round-tripped exactly the way
// maybe_sign re-serializes it (signing.cpp:315-327, :392-393). Returns false
// on a parse failure so the caller can CHECK it.
static bool build_print_frame(std::string& out)
{
    obn::json::Object pr;
    pr["command"]     = obn::json::Value(std::string("project_file"));
    pr["sequence_id"] = obn::seq_json_value("20001");
    pr["task_id"]     = obn::json::Value(std::string("task-1"));

    obn::json::Object root;
    root["print"] = obn::json::Value(std::move(pr));
    const std::string built = obn::json::Value(std::move(root)).dump();

    auto parsed = obn::json::parse(built);
    if (!parsed) return false;
    out = parsed->dump();
    return true;
}

// Write `key` as a PEM private key at `path` (ephemeral, generated below).
static bool write_test_pem(const char* path, EVP_PKEY* key)
{
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const int ok = PEM_write_PrivateKey(f, key, nullptr, nullptr, 0,
                                        nullptr, nullptr);
    std::fclose(f);
    return ok == 1;
}

// Test-local copy of the self-signed leaf bootstrap from
// tests/signing_test.cpp:207-260 (written test-locally so that file stays
// byte-identical to its Phase-6 ledger pin).
static bool write_test_slicer_cert(const char* path, EVP_PKEY* key)
{
    X509* cert = X509_new();
    if (!cert) return false;
    if (X509_set_version(cert, 2) != 1) { X509_free(cert); return false; }

    BIGNUM* bn = nullptr;
    if (BN_hex2bn(&bn, "a4e8faaa1a38e3650a0ea590d192383f") == 0 || !bn) {
        X509_free(cert);
        return false;
    }
    ASN1_INTEGER* ai = BN_to_ASN1_INTEGER(bn, nullptr);
    BN_free(bn);
    if (!ai || X509_set_serialNumber(cert, ai) != 1) {
        ASN1_INTEGER_free(ai);
        X509_free(cert);
        return false;
    }
    ASN1_INTEGER_free(ai);

    X509_NAME* name = X509_NAME_new();
    if (!name ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("GLOF3813734089.bambulab.com"),
            -1, -1, 0) != 1) {
        X509_NAME_free(name);
        X509_free(cert);
        return false;
    }
    if (X509_set_subject_name(cert, name) != 1 ||
        X509_set_issuer_name(cert, name) != 1) {
        X509_NAME_free(name);
        X509_free(cert);
        return false;
    }
    X509_NAME_free(name);

    if (!X509_gmtime_adj(X509_getm_notBefore(cert), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert), 60 * 60 * 24 * 365) ||
        X509_set_pubkey(cert, key) != 1 ||
        X509_sign(cert, key, EVP_sha256()) == 0) {
        X509_free(cert);
        return false;
    }

    std::FILE* f = std::fopen(path, "w");
    if (!f) { X509_free(cert); return false; }
    const int ok = PEM_write_X509(f, cert);
    std::fclose(f);
    X509_free(cert);
    return ok == 1;
}

// ---------------------------------------------------------------------------
// (a) flag OFF, default Settings (no config load yet in this process)
// ---------------------------------------------------------------------------

static int test_flag_off_default_settings()
{
    CHECK(!obn::config::current().exp_numeric_sequence_id);

    // Byte-identical legacy form. json_escape (print_job.cpp:114-118) adds
    // exactly two surrounding quotes for digit input — no byte in '0'..'9'
    // needs escaping — so `"20001"` is byte-equal to what json_escape yields
    // for this input domain.
    const std::string lit = obn::seq_json_literal("20001");
    CHECK(lit == "\"20001\"");

    std::string frame;
    CHECK(build_print_frame(frame));
    CHECK(frame.find("\"sequence_id\":\"20001\"") != std::string::npos);
    CHECK(frame.find("\"sequence_id\":20001") == std::string::npos);
    return 0;
}

// ---------------------------------------------------------------------------
// (b) flag OFF via a loaded conf that LACKS the key (parse-absent default)
// ---------------------------------------------------------------------------

static int test_flag_off_conf_missing_key()
{
    const fs::path dir = make_temp_dir();
    write_conf(dir / obn::config::kConfigFileName,
               "# empty experiment\nlog_level = info\n");
    obn::config::load_or_create(dir.string());

    CHECK(!obn::config::current().exp_numeric_sequence_id);

    const std::string lit = obn::seq_json_literal("20001");
    CHECK(lit == "\"20001\"");

    std::string frame;
    CHECK(build_print_frame(frame));
    CHECK(frame.find("\"sequence_id\":\"20001\"") != std::string::npos);
    CHECK(frame.find("\"sequence_id\":20001") == std::string::npos);
    return 0;
}

// ---------------------------------------------------------------------------
// (c) flag ON: bare number, Kind::Number, survives parse -> dump
// ---------------------------------------------------------------------------

static int test_flag_on_numeric()
{
    const fs::path dir = make_temp_dir();
    write_conf(dir / obn::config::kConfigFileName,
               "# EXP-01 experiment\nexp_numeric_sequence_id = 1\n");
    obn::config::load_or_create(dir.string());   // different non-empty dir

    CHECK(obn::config::current().exp_numeric_sequence_id);

    // Bare five digits: no quote characters, round-trips through std::stoul.
    const std::string lit = obn::seq_json_literal("20001");
    CHECK(lit == "20001");
    CHECK(lit.find('"') == std::string::npos);
    CHECK(std::stoul(lit) == 20001ul);

    CHECK(obn::seq_json_value("20001").kind()
          == obn::json::Value::Kind::Number);

    std::string frame;
    CHECK(build_print_frame(frame));
    CHECK(frame.find("\"sequence_id\":20001") != std::string::npos);
    CHECK(frame.find("\"sequence_id\":\"20001\"") == std::string::npos);
    return 0;
}

// ---------------------------------------------------------------------------
// (d) flag ON + signing round-trip: the number survives maybe_sign
// ---------------------------------------------------------------------------

static int test_flag_on_signing_roundtrip()
{
    CHECK(obn::config::current().exp_numeric_sequence_id);

    // Ephemeral key material written under the DEFAULT names in the SAME
    // flag-on config dir: with an empty conf value signing.cpp:41-71 resolves
    // key/cert via path_in_dir("slicer_key.pem") / path_in_dir("slicer_cert.pem"),
    // so the conf needs only the experiment key above.
    EVP_PKEY* key = EVP_RSA_gen(2048);
    CHECK(key != nullptr);

    const std::string key_path  = obn::config::path_in_dir("slicer_key.pem");
    const std::string cert_path = obn::config::path_in_dir("slicer_cert.pem");
    CHECK(!key_path.empty() && !cert_path.empty());
    CHECK(write_test_pem(key_path.c_str(), key));
    CHECK(write_test_slicer_cert(cert_path.c_str(), key));

    std::string frame;
    CHECK(build_print_frame(frame));
    CHECK(frame.find("\"sequence_id\":20001") != std::string::npos);

    const std::string env = obn::signing::maybe_sign(frame);
    CHECK(env != frame);   // a signature was actually produced

    // The number survives parse -> re-dump -> signature; no quoted twin.
    CHECK(env.find("\"sequence_id\":20001") != std::string::npos);
    CHECK(env.find("\"sequence_id\":\"20001\"") == std::string::npos);

    // Unlink the temp key material at the end.
    std::error_code ec;
    fs::remove(key_path, ec);
    fs::remove(cert_path, ec);
    EVP_PKEY_free(key);
    return 0;
}

// ---------------------------------------------------------------------------
// (e) scope pin under flag ON: the value contract is untouched
// ---------------------------------------------------------------------------

static int test_scope_pin_digits_while_flag_on()
{
    CHECK(obn::config::current().exp_numeric_sequence_id);

    // next_mqtt_seq_id() still returns the 20000-29999 digit window. The
    // security.* frames (agent.cpp:2576-2605) and the pushall constant
    // (agent.cpp:3233-3237) quote that value with their OWN literal quotes,
    // so those out-of-scope frames keep their string form while the flag is
    // ON — the flag-aware helper is used at exactly four call sites.
    const std::string v = obn::next_mqtt_seq_id();
    CHECK(v.size() == 5);
    for (char c : v) {
        CHECK(c >= '0' && c <= '9');
    }
    const unsigned long n = std::stoul(v);
    CHECK(n >= 20000ul && n <= 29999ul);
    return 0;
}

int main()
{
    if (test_flag_off_default_settings()   != 0) return 1;
    if (test_flag_off_conf_missing_key()   != 0) return 1;
    if (test_flag_on_numeric()             != 0) return 1;
    if (test_flag_on_signing_roundtrip()   != 0) return 1;
    if (test_scope_pin_digits_while_flag_on() != 0) return 1;

    std::cout << "exp_seq_test: ok\n";
    return 0;
}
