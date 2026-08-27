include_guard(GLOBAL)

set(_NEO_MACOS_BUNDLE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(neo_configure_macos_bundle target_name)
    set(_neo_one_value_args NAME IDENTIFIER ICON VERSION)
    cmake_parse_arguments(NEO_MACOS "" "${_neo_one_value_args}" "" ${ARGN})

    if(NOT APPLE)
        return()
    endif()
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "neo_configure_macos_bundle target does not exist: ${target_name}")
    endif()
    foreach(_neo_required IN ITEMS NAME IDENTIFIER)
        if(NOT NEO_MACOS_${_neo_required})
            message(FATAL_ERROR "neo_configure_macos_bundle requires ${_neo_required}")
        endif()
    endforeach()

    set(_neo_icon_name "")
    if(NEO_MACOS_ICON)
        get_filename_component(_neo_icon "${NEO_MACOS_ICON}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_neo_icon}")
            message(FATAL_ERROR "macOS bundle icon was not found: ${_neo_icon}")
        endif()
        get_filename_component(_neo_icon_name "${_neo_icon}" NAME)

        set_source_files_properties("${_neo_icon}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
        target_sources(${target_name} PRIVATE "${_neo_icon}")
    endif()

    set(_neo_bundle_version "${NEO_MACOS_VERSION}")
    if(NOT _neo_bundle_version)
        set(_neo_bundle_version "${PROJECT_VERSION}")
    endif()
    if(NOT _neo_bundle_version)
        # Some legacy applications do not yet publish a semantic version.
        # Apple bundle metadata still requires a numeric value.
        set(_neo_bundle_version "0.0.0")
    endif()

    set_target_properties(${target_name} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST
            "${_NEO_MACOS_BUNDLE_MODULE_DIR}/NeoMacOSBundleInfo.plist.in"
        MACOSX_BUNDLE_ICON_FILE "${_neo_icon_name}"
        MACOSX_BUNDLE_BUNDLE_NAME "${NEO_MACOS_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${NEO_MACOS_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${_neo_bundle_version}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${_neo_bundle_version}"
        MACOSX_BUNDLE_INFO_STRING "${NEO_MACOS_NAME} ${_neo_bundle_version}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${NEO_MACOS_NAME} ${_neo_bundle_version}"
        INSTALL_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )
endfunction()
