#pragma once

// Error/HMS decode tables - lookup API (see src/err_table.cpp for the
// adoption-site citations and src/err_table_data.cpp for the MIT-adopted
// generated data).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace obn::err {

// Normalize any HMS presentation form to the 16-lowercase-hex table key:
// "0500-0500-0001-0007" / "0500_0500_0001_0007" / "0500050000010007"
// -> "0500050000010007". Empty string if malformed (wrong length or any
// non-hex character after stripping separators).
std::string normalize_hms_key(const std::string& raw);

// Wire pair -> key. attr=0x05000500, code=0x00010007 -> "0500050000010007".
std::string hms_key(std::uint32_t attr, std::uint32_t code);

// "" if unknown (caller must then show the raw code, never hide it).
std::string describe_hms(const std::string& any_format_code);

// err_code in decimal OR 0x hex; xerr map first (84033543 etc.),
// then PandaSpy errors map via 8-hex ("83902527" -> "0500403f").
// "" if it resolves in neither family; malformed input -> "".
std::string describe_err_code(const std::string& code);

// Row counts for the adoption-site citations / count-lock tests.
std::size_t xerr_entry_count();
std::size_t hms_entry_count();
std::size_t print_error_entry_count();

// Read-only frame scan: parses a LOCAL copy of `json` (the caller's bytes
// are never touched) and returns one line per code-bearing entry found,
// in traversal order (object keys in map/lexicographic order, array
// elements in wire order). Line shapes:
//   "hms code=<16hex> -> <message-or-(unknown)>"
//   "err_code <raw> -> <message-or-(unknown)>"
// The raw code is ALWAYS in the line. No codes (or malformed JSON) ->
// empty vector (callers then emit nothing).
std::vector<std::string> scan_frame_codes(const std::string& json);

// Generated-data access (src/err_table_data.cpp); exposed here so the
// count/sweep tests and the lookup implementation share one declaration.
namespace data {
struct Entry {
    const char* key;
    const char* message;
};
extern const Entry hms_entries[];
extern const std::size_t hms_count;
extern const Entry print_error_entries[];
extern const std::size_t print_error_count;
} // namespace data

} // namespace obn::err
