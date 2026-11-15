# print_bot core: GUI-free application core (presets, project, slicing, events).
# Must never include or link wxWidgets; check_no_gui.cmake enforces this on every build.
cmake_minimum_required(VERSION 3.13)
project(printbot_core)

add_custom_target(printbot_no_gui_check
    COMMAND ${CMAKE_COMMAND} "-DDIRS=${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/../mcp" -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_gui.cmake
    COMMENT "Checking that print_bot core/MCP sources are GUI-free"
    VERBATIM)

add_library(printbot_core STATIC
    Core.cpp Core.hpp
    EventBus.cpp EventBus.hpp
    Executor.cpp Executor.hpp
    Json.cpp Json.hpp
    PresetService.cpp PresetService.hpp
    ProjectService.cpp ProjectService.hpp
    Result.hpp
    SliceEvents.hpp
    SliceService.cpp SliceService.hpp
    Snapshots.hpp
    Version.hpp)
add_dependencies(printbot_core printbot_no_gui_check)
target_include_directories(printbot_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(printbot_core PUBLIC libslic3r)
