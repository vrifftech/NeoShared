include_guard(GLOBAL)

# Read a semantic application version from a source Version.hpp file.
# The header must contain exactly one line of the form:
#   #define <macro_name> "MAJOR.MINOR.PATCH"
function(neo_read_version_header out_variable header_path macro_name)
    if(NOT EXISTS "${header_path}")
        message(FATAL_ERROR "Version header was not found: ${header_path}")
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header_path}")

    file(STRINGS "${header_path}" _neo_version_lines
        REGEX "^[ \t]*#define[ \t]+${macro_name}[ \t]+\"[0-9]+\\.[0-9]+\\.[0-9]+\"[ \t]*$")

    list(LENGTH _neo_version_lines _neo_version_line_count)
    if(NOT _neo_version_line_count EQUAL 1)
        message(FATAL_ERROR
            "Expected exactly one ${macro_name} semantic-version definition in "
            "'${header_path}', found ${_neo_version_line_count}.")
    endif()

    list(GET _neo_version_lines 0 _neo_version_line)
    string(REGEX MATCH "\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\"" _neo_version_match
        "${_neo_version_line}")
    if(NOT _neo_version_match)
        message(FATAL_ERROR
            "Unable to parse ${macro_name} from '${header_path}'.")
    endif()

    set(${out_variable}
        "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}"
        PARENT_SCOPE)
endfunction()

# Configure a Windows resource template after project(...) has established
# PROJECT_VERSION and its component variables. The generated resource lives in
# the build tree so the source template never duplicates the version number.
function(neo_configure_windows_version_resource out_variable template_path output_name icon_path)
    if(NOT WIN32)
        set(${out_variable} "" PARENT_SCOPE)
        return()
    endif()

    if(NOT EXISTS "${template_path}")
        message(FATAL_ERROR "Windows version-resource template was not found: ${template_path}")
    endif()
    if(NOT EXISTS "${icon_path}")
        message(FATAL_ERROR "Windows application icon was not found: ${icon_path}")
    endif()

    file(TO_CMAKE_PATH "${icon_path}" NEO_VERSION_RESOURCE_ICON)
    set(_neo_version_resource_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_neo_version_resource_dir}")
    set(_neo_version_resource "${_neo_version_resource_dir}/${output_name}")

    configure_file("${template_path}" "${_neo_version_resource}" @ONLY NEWLINE_STYLE CRLF)
    set(${out_variable} "${_neo_version_resource}" PARENT_SCOPE)
endfunction()
