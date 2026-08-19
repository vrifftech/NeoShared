# NeoTools Emscripten compatibility wrapper for CMake's stock FindwxWidgets.
#
# The Emscripten toolchain intentionally sets CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
# and CMAKE_FIND_ROOT_PATH_MODE_INCLUDE to ONLY. The wxWidgets-WASM build lives
# outside the SDK sysroot, and wx-config reports that external include/lib
# directory. CMake's stock module subsequently validates the reported headers
# and every -l entry with find_file()/find_library(); with the default root
# modes, those valid external paths are re-rooted into the SDK sysroot and the
# package is rejected.
#
# Temporarily allow the explicit wx-config paths while the stock module runs,
# then restore the toolchain's isolation for every other package lookup.

set(_neo_wx_saved_root_mode_include "${CMAKE_FIND_ROOT_PATH_MODE_INCLUDE}")
set(_neo_wx_saved_root_mode_library "${CMAKE_FIND_ROOT_PATH_MODE_LIBRARY}")

if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    message(STATUS
        "Using NeoTools wxWidgets-WASM discovery compatibility wrapper")
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
endif()

include("${CMAKE_ROOT}/Modules/FindwxWidgets.cmake")

if(_neo_wx_saved_root_mode_include STREQUAL "")
    unset(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE)
else()
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE "${_neo_wx_saved_root_mode_include}")
endif()

if(_neo_wx_saved_root_mode_library STREQUAL "")
    unset(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY)
else()
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY "${_neo_wx_saved_root_mode_library}")
endif()

unset(_neo_wx_saved_root_mode_include)
unset(_neo_wx_saved_root_mode_library)
