include_guard(GLOBAL)

# Shared WebAssembly integration for the independent NeoTools applications.
# The wxWidgets DOM port and Emscripten SDK are external build dependencies;
# application repositories continue to depend only on neoshared source targets.

set(NEO_WASM_EMSCRIPTEN_VERSION "4.0.2" CACHE STRING
    "Pinned Emscripten SDK version used by the NeoTools browser builds")
set(NEO_WASM_WX_COMMIT "bca69b9fddc88adec57b05e6809467ef9f5158c8" CACHE STRING
    "Pinned PCBJam/wxWidgets wasm-port commit")
set(NEO_WX_WASM_SOURCE "" CACHE PATH
    "Path to the pinned PCBJam/wxWidgets wasm-port checkout")
set(NEO_WX_WASM_BUILD "" CACHE PATH
    "Path to the configured wxWidgets-WASM build directory")

get_filename_component(_NEO_WASM_SHARED_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Register a narrow compatibility wrapper for CMake's stock FindwxWidgets.
# This file is included before project(), so the Emscripten toolchain may not
# have set EMSCRIPTEN/CMAKE_SYSTEM_NAME yet. Register the wrapper unconditionally;
# it delegates unchanged on native builds and adjusts root-search modes only
# after the toolchain identifies an Emscripten configure.
list(PREPEND CMAKE_MODULE_PATH "${_NEO_WASM_SHARED_ROOT}/cmake/wasm")

function(neo_configure_wasm_target target_name)
    set(_one_value_args NAME SLUG ICON)
    cmake_parse_arguments(NEO_WASM "" "${_one_value_args}" "" ${ARGN})

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

    set(NEO_WASM_PAGE_TITLE "${NEO_WASM_NAME} ${PROJECT_VERSION}")
    set(NEO_WASM_APP_NAME "${NEO_WASM_NAME}")
    set(NEO_WASM_APP_VERSION "${PROJECT_VERSION}")
    set(NEO_WASM_APP_SLUG "${NEO_WASM_SLUG}")
    set(_wasm_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/wasm-generated/${target_name}")
    set(_wasm_output_dir "${CMAKE_BINARY_DIR}/wasm-output")
    file(MAKE_DIRECTORY "${_wasm_generated_dir}" "${_wasm_output_dir}")
    set(_shell_file "${_wasm_generated_dir}/${NEO_WASM_SLUG}-shell.html")
    configure_file("${_shell_template}" "${_shell_file}" @ONLY)

    target_sources(${target_name} PRIVATE
        "${_NEO_WASM_SHARED_ROOT}/wx/NeoBrowserFiles.cpp"
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
        "SHELL:-sERROR_ON_UNDEFINED_SYMBOLS=0"
        "SHELL:-sASYNCIFY=1"
        "SHELL:-sASYNCIFY_STACK_SIZE=65536"
        "-sASYNCIFY_IMPORTS=startModal,js_writeTextToClipboard,js_readTextFromClipboard,js_clipboardHasText,js_clearClipboard,js_enumerateFonts"
        "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP8,HEAP32,ccall,FS"
        "SHELL:-lidbfs.js"
        "SHELL:--pre-js ${_neo_pre_js}"
        "SHELL:--pre-js ${_wx_pre_js}"
        "SHELL:--pre-js ${_wx_dom_pre_js}"
        "SHELL:--shell-file ${_shell_file}"
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
