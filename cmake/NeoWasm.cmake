include_guard(GLOBAL)

# Shared WebAssembly integration for the independent NeoTools applications.
# The wxWidgets DOM port and Emscripten SDK are external build dependencies;
# application repositories continue to depend only on neoshared source targets.

set(_NEO_WASM_VERSIONS_FILE "${CMAKE_CURRENT_LIST_DIR}/../wasm/versions.env")
if(NOT EXISTS "${_NEO_WASM_VERSIONS_FILE}")
    message(FATAL_ERROR "NeoTools WebAssembly version manifest is missing: ${_NEO_WASM_VERSIONS_FILE}")
endif()

function(_neo_wasm_read_version key output)
    file(STRINGS "${_NEO_WASM_VERSIONS_FILE}" _matches REGEX "^${key}=")
    list(LENGTH _matches _match_count)
    if(NOT _match_count EQUAL 1)
        message(FATAL_ERROR "Expected exactly one ${key}= entry in ${_NEO_WASM_VERSIONS_FILE}")
    endif()
    list(GET _matches 0 _line)
    string(REGEX REPLACE "^[^=]+=" "" _value "${_line}")
    if(_value STREQUAL "")
        message(FATAL_ERROR "${key} is empty in ${_NEO_WASM_VERSIONS_FILE}")
    endif()
    set(${output} "${_value}" PARENT_SCOPE)
endfunction()

_neo_wasm_read_version(NEO_WASM_EMSCRIPTEN_VERSION _neo_wasm_pinned_emscripten)
_neo_wasm_read_version(NEO_WASM_WX_COMMIT _neo_wasm_pinned_wx_commit)

set(NEO_WASM_EMSCRIPTEN_VERSION "${_neo_wasm_pinned_emscripten}" CACHE STRING
    "Pinned Emscripten SDK version used by the NeoTools browser builds")
set(NEO_WASM_WX_COMMIT "${_neo_wasm_pinned_wx_commit}" CACHE STRING
    "Pinned PCBJam/wxWidgets wasm-port commit")
if(NOT "${NEO_WASM_EMSCRIPTEN_VERSION}" STREQUAL "${_neo_wasm_pinned_emscripten}" OR
   NOT "${NEO_WASM_WX_COMMIT}" STREQUAL "${_neo_wasm_pinned_wx_commit}")
    message(FATAL_ERROR
        "WebAssembly dependency overrides do not match neoshared/wasm/versions.env")
endif()
set(NEO_WX_WASM_SOURCE "" CACHE PATH
    "Path to the pinned PCBJam/wxWidgets wasm-port checkout")
set(NEO_WX_WASM_BUILD "" CACHE PATH
    "Path to the configured wxWidgets-WASM build directory")

get_filename_component(_NEO_WASM_SHARED_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Register a narrow compatibility wrapper for CMake's stock FindwxWidgets.
# This file can be included before project(), so the Emscripten toolchain may
# not have set EMSCRIPTEN/CMAKE_SYSTEM_NAME yet. Register the wrapper
# unconditionally; it delegates unchanged on native builds and adjusts root
# search modes only after the toolchain identifies an Emscripten configure.
list(PREPEND CMAKE_MODULE_PATH "${_NEO_WASM_SHARED_ROOT}/cmake/wasm")

function(neo_configure_wasm_target target_name)
    set(_one_value_args NAME SLUG ICON VERSION)
    set(_multi_value_args PRELOAD)
    cmake_parse_arguments(NEO_WASM "" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    if(NOT EMSCRIPTEN)
        return()
    endif()
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "neo_configure_wasm_target target does not exist: ${target_name}")
    endif()
    if(NOT NEO_WASM_NAME OR NOT NEO_WASM_SLUG OR NOT NEO_WASM_ICON)
        message(FATAL_ERROR "neo_configure_wasm_target requires NAME, SLUG, and ICON")
    endif()
    if(NOT NEO_WX_WASM_SOURCE)
        message(FATAL_ERROR
            "NEO_WX_WASM_SOURCE is required for browser builds. Use the shared "
            "scripts/build-wasm-app.sh helper or set it explicitly.")
    endif()

    get_filename_component(_wx_source "${NEO_WX_WASM_SOURCE}" ABSOLUTE)
    get_filename_component(_icon_source "${NEO_WASM_ICON}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_wx_pre_js "${_wx_source}/build/wasm/wx.js")
    set(_wx_dom_pre_js "${_wx_source}/build/wasm/wx-dom.js")
    set(_neo_pre_js "${_NEO_WASM_SHARED_ROOT}/wasm/neo-pre.js")
    set(_shell_template "${_NEO_WASM_SHARED_ROOT}/wasm/neo-shell.html.in")

    foreach(_required_file IN ITEMS
        "${_wx_pre_js}"
        "${_wx_dom_pre_js}"
        "${_neo_pre_js}"
        "${_shell_template}"
        "${_icon_source}")
        if(NOT EXISTS "${_required_file}")
            message(FATAL_ERROR "Required WebAssembly build input was not found: ${_required_file}")
        endif()
    endforeach()

    set(_app_version "${NEO_WASM_VERSION}")
    if(NOT _app_version)
        set(_app_version "${PROJECT_VERSION}")
    endif()
    if(NOT _app_version)
        set(_app_version "snapshot")
    endif()

    set(_preload_link_options)
    foreach(_preload_entry IN LISTS NEO_WASM_PRELOAD)
        string(FIND "${_preload_entry}" "@" _preload_separator)
        if(_preload_separator LESS 1)
            message(FATAL_ERROR
                "WebAssembly PRELOAD entries must use source@/virtual/path syntax: ${_preload_entry}")
        endif()
        string(SUBSTRING "${_preload_entry}" 0 ${_preload_separator} _preload_source)
        math(EXPR _preload_destination_begin "${_preload_separator} + 1")
        string(SUBSTRING "${_preload_entry}" ${_preload_destination_begin} -1 _preload_destination)
        if(NOT _preload_destination MATCHES "^/")
            message(FATAL_ERROR
                "WebAssembly PRELOAD destinations must be absolute virtual paths: ${_preload_entry}")
        endif()
        get_filename_component(_preload_source_absolute "${_preload_source}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_preload_source_absolute}")
            message(FATAL_ERROR
                "WebAssembly preload source was not found: ${_preload_source_absolute}")
        endif()
        list(APPEND _preload_link_options
            "SHELL:--preload-file \"${_preload_source_absolute}@${_preload_destination}\"")
    endforeach()

    set(NEO_WASM_PAGE_TITLE "${NEO_WASM_NAME} ${_app_version}")
    set(NEO_WASM_APP_NAME "${NEO_WASM_NAME}")
    set(NEO_WASM_APP_VERSION "${_app_version}")
    set(NEO_WASM_APP_SLUG "${NEO_WASM_SLUG}")
    set(_wasm_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/wasm-generated/${target_name}")
    set(_wasm_output_dir "${CMAKE_BINARY_DIR}/wasm-output")
    file(MAKE_DIRECTORY "${_wasm_generated_dir}" "${_wasm_output_dir}")
    set(_shell_file "${_wasm_generated_dir}/${NEO_WASM_SLUG}-shell.html")
    configure_file("${_shell_template}" "${_shell_file}" @ONLY)

    target_sources(${target_name} PRIVATE
        "${_NEO_WASM_SHARED_ROOT}/wx/NeoBrowserFiles.cpp"
        "${_NEO_WASM_SHARED_ROOT}/src/wasm_dialog_compat.cpp"
    )

    target_include_directories(${target_name} PRIVATE
        "${_NEO_WASM_SHARED_ROOT}/include"
    )

    target_compile_definitions(${target_name} PRIVATE
        NEO_WASM=1
        NEO_BROWSER_BUILD=1
    )
    # Use Emscripten's JavaScript exception model. It is slower than native
    # wasm-EH, but it can be combined with in-link Asyncify without the custom
    # post-link Binaryen pipeline required by the PCBJam/KiCad production build.
    target_compile_options(${target_name} PRIVATE
        -fexceptions
        -Wno-unused-command-line-argument
    )

    target_link_options(${target_name} PRIVATE
        -fexceptions
        "SHELL:-sSUPPORT_LONGJMP=emscripten"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sINITIAL_MEMORY=134217728"
        "SHELL:-sMAXIMUM_MEMORY=2147483648"
        "SHELL:-sSTACK_SIZE=4194304"
        "SHELL:-sABORTING_MALLOC=0"
        "SHELL:-sEXIT_RUNTIME=0"
        "SHELL:-sENVIRONMENT=web"
        "SHELL:-sFORCE_FILESYSTEM=1"
        "SHELL:-sUSE_ZLIB=1"
        "SHELL:-sERROR_ON_UNDEFINED_SYMBOLS=1"
        "SHELL:-Wl,--undefined=neo_wasm_dialog_compat_link_anchor"
        "SHELL:-Wl,--undefined=neo_browser_open_files_completed"
        "SHELL:-Wl,--undefined=neo_browser_retained_file_set_completed"
        "SHELL:-Wl,--undefined=neo_browser_retained_export_completed"
        "SHELL:-Wl,--undefined=neo_browser_download_completed"
        "SHELL:-Wl,--undefined=neo_browser_package_directory_completed"
        "SHELL:-Wl,--undefined=neo_browser_package_workspace_completed"
        "SHELL:-Wl,--undefined=neo_browser_package_commit_completed"
        "SHELL:-sASYNCIFY=1"
        "SHELL:-sASYNCIFY_STACK_SIZE=1048576"
        "-sASYNCIFY_IMPORTS=startModal,js_writeTextToClipboard,js_readTextFromClipboard,js_clipboardHasText,js_clearClipboard,js_enumerateFonts"
        "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP8,HEAP32,ccall,FS"
        "SHELL:-lidbfs.js"
        "SHELL:--pre-js \"${_neo_pre_js}\""
        "SHELL:--pre-js \"${_wx_pre_js}\""
        "SHELL:--pre-js \"${_wx_dom_pre_js}\""
        "SHELL:--shell-file \"${_shell_file}\""
        ${_preload_link_options}
    )

    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME "${NEO_WASM_SLUG}"
        SUFFIX ".html"
        RUNTIME_OUTPUT_DIRECTORY "${_wasm_output_dir}"
    )

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_icon_source}" "${_wasm_output_dir}/${NEO_WASM_SLUG}.svg"
        VERBATIM
    )
endfunction()
