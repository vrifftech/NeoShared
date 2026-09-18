include_guard(GLOBAL)

# Remember the module location independently of the calling directory. The
# collection still supports CMake 3.16, which predates
# CMAKE_CURRENT_FUNCTION_LIST_DIR.
set_property(GLOBAL PROPERTY NEOSHARED_NEO_VERSION_MODULE_DIR
    "${CMAKE_CURRENT_LIST_DIR}")

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

function(_neo_normalize_windows_version out_string out_numeric version label)
    if("${version}" STREQUAL "")
        message(FATAL_ERROR "${label} must not be empty.")
    endif()

    string(REPLACE "." ";" _neo_version_parts "${version}")
    list(LENGTH _neo_version_parts _neo_version_count)
    if(_neo_version_count LESS 1 OR _neo_version_count GREATER 4)
        message(FATAL_ERROR
            "${label} '${version}' must contain one to four numeric components.")
    endif()

    foreach(_neo_component IN LISTS _neo_version_parts)
        if(NOT _neo_component MATCHES "^[0-9]+$")
            message(FATAL_ERROR
                "${label} '${version}' contains a non-numeric component.")
        endif()
        if(_neo_component GREATER 65535)
            message(FATAL_ERROR
                "${label} '${version}' contains a component greater than 65535.")
        endif()
    endforeach()

    while(_neo_version_count LESS 4)
        list(APPEND _neo_version_parts 0)
        math(EXPR _neo_version_count "${_neo_version_count} + 1")
    endwhile()

    list(GET _neo_version_parts 0 _neo_version_0)
    list(GET _neo_version_parts 1 _neo_version_1)
    list(GET _neo_version_parts 2 _neo_version_2)
    list(GET _neo_version_parts 3 _neo_version_3)

    set(${out_string} "${version}" PARENT_SCOPE)
    set(${out_numeric}
        "${_neo_version_0},${_neo_version_1},${_neo_version_2},${_neo_version_3}"
        PARENT_SCOPE)
endfunction()

function(_neo_escape_windows_resource_string out_variable value)
    set(_neo_escaped "${value}")
    string(REPLACE "\\" "\\\\" _neo_escaped "${_neo_escaped}")
    string(REPLACE "\"" "\\\"" _neo_escaped "${_neo_escaped}")
    string(REPLACE "\r" " " _neo_escaped "${_neo_escaped}")
    string(REPLACE "\n" " " _neo_escaped "${_neo_escaped}")
    set(${out_variable} "${_neo_escaped}" PARENT_SCOPE)
endfunction()

# Attach a generated VERSIONINFO resource to an existing executable target.
#
# Version ownership remains with the calling application. VERSION normally
# receives that repository's PROJECT_VERSION. The shared defaults are limited
# to the collection-wide publisher/copyright strings.
#
# Required in practice:
#   neo_add_windows_version_resource(MyTarget
#       VERSION "${PROJECT_VERSION}"
#       PRODUCT_NAME "My Product"
#       FILE_DESCRIPTION "My Product description"
#       INTERNAL_NAME "MyTarget"
#       ORIGINAL_FILENAME "MyTarget.exe"
#       ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/my.ico")
#
# ICON is optional, which is useful for command-line programs and targets that
# already link a separate resource file containing manifests or other assets.
function(neo_add_windows_version_resource target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Cannot add Windows version metadata: target '${target_name}' does not exist.")
    endif()

    get_target_property(_neo_target_type "${target_name}" TYPE)
    if(NOT _neo_target_type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR
            "Windows version metadata can only be attached to an executable target; "
            "'${target_name}' is ${_neo_target_type}.")
    endif()

    set(_neo_one_value_args
        VERSION
        PRODUCT_VERSION
        PRODUCT_NAME
        FILE_DESCRIPTION
        INTERNAL_NAME
        ORIGINAL_FILENAME
        ICON
        ICON_ID
        COMPANY_NAME
        LEGAL_COPYRIGHT)
    cmake_parse_arguments(NEO_WINDOWS "" "${_neo_one_value_args}" "" ${ARGN})

    if(NEO_WINDOWS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown neo_add_windows_version_resource arguments for '${target_name}': "
            "${NEO_WINDOWS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT NEO_WINDOWS_VERSION)
        if(PROJECT_VERSION)
            set(NEO_WINDOWS_VERSION "${PROJECT_VERSION}")
        else()
            message(FATAL_ERROR
                "VERSION is required for '${target_name}' because PROJECT_VERSION is empty.")
        endif()
    endif()
    if(NOT NEO_WINDOWS_PRODUCT_VERSION)
        set(NEO_WINDOWS_PRODUCT_VERSION "${NEO_WINDOWS_VERSION}")
    endif()

    get_target_property(_neo_output_name "${target_name}" OUTPUT_NAME)
    if(NOT _neo_output_name OR _neo_output_name MATCHES "-NOTFOUND$")
        set(_neo_output_name "${target_name}")
    endif()

    if(NOT NEO_WINDOWS_PRODUCT_NAME)
        set(NEO_WINDOWS_PRODUCT_NAME "${_neo_output_name}")
    endif()
    if(NOT NEO_WINDOWS_FILE_DESCRIPTION)
        set(NEO_WINDOWS_FILE_DESCRIPTION "${NEO_WINDOWS_PRODUCT_NAME}")
    endif()
    if(NOT NEO_WINDOWS_INTERNAL_NAME)
        set(NEO_WINDOWS_INTERNAL_NAME "${_neo_output_name}")
    endif()
    if(NOT NEO_WINDOWS_ORIGINAL_FILENAME)
        set(NEO_WINDOWS_ORIGINAL_FILENAME "${_neo_output_name}.exe")
    endif()
    if(NOT NEO_WINDOWS_COMPANY_NAME)
        set(NEO_WINDOWS_COMPANY_NAME "Vriff")
    endif()
    if(NOT NEO_WINDOWS_LEGAL_COPYRIGHT)
        set(NEO_WINDOWS_LEGAL_COPYRIGHT "NeoTools Project")
    endif()

    _neo_normalize_windows_version(
        NEO_WINDOWS_FILE_VERSION
        NEO_WINDOWS_FILE_VERSION_NUMERIC
        "${NEO_WINDOWS_VERSION}"
        "File version")
    _neo_normalize_windows_version(
        NEO_WINDOWS_PRODUCT_VERSION
        NEO_WINDOWS_PRODUCT_VERSION_NUMERIC
        "${NEO_WINDOWS_PRODUCT_VERSION}"
        "Product version")

    foreach(_neo_field IN ITEMS
        COMPANY_NAME
        FILE_DESCRIPTION
        FILE_VERSION
        INTERNAL_NAME
        LEGAL_COPYRIGHT
        ORIGINAL_FILENAME
        PRODUCT_NAME
        PRODUCT_VERSION)
        _neo_escape_windows_resource_string(
            NEO_WINDOWS_${_neo_field}
            "${NEO_WINDOWS_${_neo_field}}")
    endforeach()

    set(NEO_WINDOWS_ICON_RESOURCE "")
    if(NEO_WINDOWS_ICON)
        get_filename_component(_neo_icon_path "${NEO_WINDOWS_ICON}"
            ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_neo_icon_path}")
            message(FATAL_ERROR
                "Windows application icon was not found for '${target_name}': "
                "${_neo_icon_path}")
        endif()
        if(NOT NEO_WINDOWS_ICON_ID)
            set(NEO_WINDOWS_ICON_ID IDI_APP_ICON)
        endif()
        if(NOT NEO_WINDOWS_ICON_ID MATCHES "^([A-Za-z_][A-Za-z0-9_]*|[0-9]+)$")
            message(FATAL_ERROR
                "ICON_ID '${NEO_WINDOWS_ICON_ID}' for '${target_name}' is not a valid "
                "Windows resource identifier.")
        endif()
        file(TO_CMAKE_PATH "${_neo_icon_path}" _neo_icon_path)
        _neo_escape_windows_resource_string(_neo_icon_path "${_neo_icon_path}")
        set(NEO_WINDOWS_ICON_RESOURCE
            "${NEO_WINDOWS_ICON_ID} ICON \"${_neo_icon_path}\"")
    endif()

    get_property(_neo_version_module_dir GLOBAL PROPERTY
        NEOSHARED_NEO_VERSION_MODULE_DIR)
    if(NOT _neo_version_module_dir)
        message(FATAL_ERROR "NeoVersion.cmake could not resolve its template directory.")
    endif()

    string(MAKE_C_IDENTIFIER "${target_name}" _neo_target_identifier)
    set(_neo_version_resource_dir
        "${CMAKE_CURRENT_BINARY_DIR}/generated/windows-version")
    file(MAKE_DIRECTORY "${_neo_version_resource_dir}")
    set(_neo_version_resource
        "${_neo_version_resource_dir}/${_neo_target_identifier}_version.rc")

    configure_file(
        "${_neo_version_module_dir}/NeoWindowsVersion.rc.in"
        "${_neo_version_resource}"
        @ONLY
        NEWLINE_STYLE CRLF)
    set_source_files_properties("${_neo_version_resource}" PROPERTIES GENERATED TRUE)
    target_sources("${target_name}" PRIVATE "${_neo_version_resource}")
endfunction()

# Legacy helper retained for source compatibility with repositories that have
# not yet migrated to neo_add_windows_version_resource(). New code should use
# the target-based generator above.
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
