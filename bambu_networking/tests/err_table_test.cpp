// Tests for src/err_table.cpp - xerr/HMS lookup, format normalization,
// and the read-only frame code scan.
// Framework: repo convention (int main() + fail_count + CHECK, as in
// push_status_merge_test.cpp).
//
// Provenance locks (07-01 must_haves):
//  - hms_entry_count()==3958 and print_error_entry_count()==539 pin the
//    full MIT PandaSpy tables generated from the SHA-256-pinned en.json
//    (B7FD5358F3163B9B424FF4CD8D2A1F1B15519EFC5CFCA3D8ED31378CE05F3B64).
//  - describe_err_code consults the xerr map first and the print_error
//    map second (adjacency pin below).

#include "obn/err_table.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int fail_count = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++fail_count;                                               \
        }                                                               \
    } while (0)

int main() {
    // ---- Canonical key + normalization equality (encoding edge probe):
    // hyphen, underscore, uppercase and bare presentations all collapse
    // to the single 16-lowercase-hex key.
    CHECK(obn::err::hms_key(0x05000500u, 0x00010007u) == "0500050000010007");
    CHECK(obn::err::normalize_hms_key("0500-0500-0001-0007") == "0500050000010007");
    CHECK(obn::err::normalize_hms_key("0500_0500_0001_0007") == "0500050000010007");
    CHECK(obn::err::normalize_hms_key("0500050000010007") == "0500050000010007");
    CHECK(obn::err::normalize_hms_key("0500-0500-0001-00AB") == "05000500000100ab");
    CHECK(obn::err::normalize_hms_key("0500-0500-0001-000").empty());  // short
    CHECK(obn::err::normalize_hms_key("zz00050000010007").empty());    // non-hex

    // ---- HMS lookups: the two mandated fixture keys decode non-empty.
    CHECK(!obn::err::describe_hms("0500050000010007").empty());
    CHECK(!obn::err::describe_hms("0300020000010001").empty());
    // The three presentation forms resolve through the same table entry.
    CHECK(obn::err::describe_hms("0500-0500-0001-0007") ==
          obn::err::describe_hms("0500050000010007"));
    CHECK(obn::err::describe_hms("0500_0500_0001_0007") ==
          obn::err::describe_hms("0500050000010007"));
    // Empty and unknown inputs return "" (caller shows the raw code).
    CHECK(obn::err::describe_hms("").empty());
    CHECK(obn::err::describe_hms("7fff00000001ffff").empty());  // unknown 16-hex

    // ---- err_code lookups: seeded xerr rows + decimal->hex print_error.
    CHECK(!obn::err::describe_err_code("84033543").empty());
    CHECK(!obn::err::describe_err_code("84033545").empty());
    CHECK(!obn::err::describe_err_code("1035").empty());   // generic xerr REST row
    CHECK(!obn::err::describe_err_code("83902527").empty()); // -> 0500403f
    CHECK(obn::err::describe_err_code("").empty());
    CHECK(obn::err::describe_err_code("not-a-code").empty()); // malformed

    // Adjacency pin: describe_err_code consults the xerr map FIRST and
    // the print_error map second, so for a code present in both
    // families the xerr paraphrase wins deterministically. The two
    // branches are pinned by string equality on the xerr answer and by
    // the print_error fallback below (no natural code overlap exists
    // between the families in the data - swept at plan time - so the
    // pin is: the xerr branch's exact answer is what a caller sees, and
    // the print_error branch is only consulted when xerr misses).
    CHECK(obn::err::describe_err_code("84033543") ==
          std::string("The printer rejected an MQTT command because its "
                      "per-command signature was missing or invalid."));
    CHECK(!obn::err::describe_err_code("83902527").empty());  // print_error branch

    // ---- Count locks (full-table adoption, SHA-pinned provenance).
    CHECK(obn::err::hms_entry_count() == 3958);
    CHECK(obn::err::print_error_entry_count() == 539);

    // 113 = 111 REST rows (the 111-entry source table, ERR-01 criterion 1) + 2 MQTT-security rows
    CHECK(obn::err::xerr_entry_count() == 113);

    // Reused code 1018 (source-row order): both meanings surface through
    // the "; " join, and the second MQTT-security row decodes too.
    const std::string code1018 = obn::err::describe_err_code("1018");
    CHECK(!code1018.empty());
    CHECK(code1018.find("; ") != std::string::npos);
    CHECK(!obn::err::describe_err_code("84033545").empty());

    // ---- Sweep: every generated message is non-empty.
    for (std::size_t i = 0; i < obn::err::hms_entry_count(); ++i) {
        const obn::err::data::Entry& e = obn::err::data::hms_entries[i];
        CHECK(e.key != nullptr && e.key[0] != '\0');
        CHECK(e.message != nullptr && e.message[0] != '\0');
    }
    for (std::size_t i = 0; i < obn::err::print_error_entry_count(); ++i) {
        const obn::err::data::Entry& e = obn::err::data::print_error_entries[i];
        CHECK(e.key != nullptr && e.key[0] != '\0');
        CHECK(e.message != nullptr && e.message[0] != '\0');
    }

    // ---- scan_frame_codes (ordering edge probe): top-level err_code
    // first (lexicographic key order), then the two hms array entries in
    // wire order; two scans of the same frame are identical.
    const std::string frame =
        "{\"err_code\":84033543,\"hms\":[{\"attr\":83887360,\"code\":65543},"
        "{\"attr\":\"03000200\",\"code\":\"00010001\"}]}";
    const std::vector<std::string> lines = obn::err::scan_frame_codes(frame);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[0].rfind("err_code 84033543 -> ", 0) == 0);
        CHECK(lines[1].rfind("hms code=0500050000010007 -> ", 0) == 0);
        CHECK(lines[2].rfind("hms code=0300020000010001 -> ", 0) == 0);
        // Decoded (non-empty) messages, raw code always present.
        CHECK(lines[0].find("(unknown)") == std::string::npos);
        CHECK(lines[1].find("(unknown)") == std::string::npos);
        CHECK(lines[2].find("(unknown)") == std::string::npos);
    }
    const std::vector<std::string> again = obn::err::scan_frame_codes(frame);
    CHECK(again == lines);

    // Unknown code in a frame: line still carries the raw code and the
    // (unknown) marker; nothing is hidden.
    const std::vector<std::string> unknown_lines =
        obn::err::scan_frame_codes("{\"err_code\":999999999999}");
    CHECK(unknown_lines.size() == 1);
    if (!unknown_lines.empty()) {
        CHECK(unknown_lines[0].find("999999999999") != std::string::npos);
        CHECK(unknown_lines[0].find("(unknown)") != std::string::npos);
    }

    // Empty vector for code-less or malformed frames (no log line).
    CHECK(obn::err::scan_frame_codes("{}").empty());
    CHECK(obn::err::scan_frame_codes("{\"print\":\"push_status\"}").empty());
    CHECK(obn::err::scan_frame_codes("not json {{").empty());
    CHECK(obn::err::scan_frame_codes("").empty());

    if (fail_count) {
        std::fprintf(stderr, "err_table_test: %d check(s) FAILED\n", fail_count);
        return 1;
    }
    std::printf("err_table_test: all checks passed\n");
    return 0;
}
