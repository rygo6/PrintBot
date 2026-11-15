// print_bot core: JSON views of core snapshots (used by the MCP server and events).
#ifndef printbot_core_Json_hpp_
#define printbot_core_Json_hpp_

#include <nlohmann/json.hpp>

#include "Snapshots.hpp"

namespace printbot::core {

nlohmann::json to_json(const Error &e);
nlohmann::json to_json(const PresetSnapshot &s);
nlohmann::json to_json(const ObjectInfo &o);
nlohmann::json to_json(const ProjectSnapshot &s);
nlohmann::json to_json(const SliceSummary &s);
nlohmann::json to_json(const SliceSnapshot &s);

} // namespace printbot::core

#endif
