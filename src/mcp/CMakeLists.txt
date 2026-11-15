# print_bot MCP server (stdio). GUI-free: links only the core + libslic3r.
cmake_minimum_required(VERSION 3.13)
project(printbot_mcp)

add_library(printbot_mcp STATIC McpServer.cpp McpServer.hpp McpArgs.hpp McpSocket.cpp McpSocket.hpp McpTools.cpp Subprocess.cpp Subprocess.hpp)
target_link_libraries(printbot_mcp PUBLIC printbot_core)

# libslic3r references a few symbols defined outside it (Slic3r::Http used by the encrypted
# log sink, BBL_Encrypt, the nanosvg parser); the GUI app gets them from libslic3r_gui. The
# headless server compiles just those GUI-free files instead of linking the GUI library.
add_library(printbot_slic3r_support STATIC
    ../slic3r/Utils/Http.cpp
    ../slic3r/Utils/BBLUtil.cpp
    NanoSvgImpl.cpp)
target_link_libraries(printbot_slic3r_support PUBLIC libslic3r libcurl OpenSSL::SSL OpenSSL::Crypto)

add_executable(print_bot_mcp main.cpp)
set_target_properties(print_bot_mcp PROPERTIES
    OUTPUT_NAME "print_bot-mcp"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/..")
target_link_libraries(print_bot_mcp printbot_mcp printbot_slic3r_support)
if (NOT WIN32)
    target_link_libraries(print_bot_mcp ${CMAKE_DL_LIBS} Threads::Threads)
endif ()
if (MINGW)
    target_link_options(print_bot_mcp PUBLIC "-Wl,-allow-multiple-definition")
endif ()
install(TARGETS print_bot_mcp RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
