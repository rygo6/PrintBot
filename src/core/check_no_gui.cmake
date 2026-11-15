# Fails the build if any print_bot core/MCP source includes wxWidgets or slic3r/GUI headers.
# Usage: cmake -DDIRS="dir1;dir2" -P check_no_gui.cmake
foreach(_dir ${DIRS})
    file(GLOB _files "${_dir}/*.cpp" "${_dir}/*.hpp" "${_dir}/*.h")
    foreach(_f ${_files})
        file(STRINGS "${_f}" _hits REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](wx/|slic3r/GUI/|slic3r/Utils/)")
        if(_hits)
            message(FATAL_ERROR "print_bot core must stay GUI-free: ${_f}: ${_hits}")
        endif()
    endforeach()
endforeach()
