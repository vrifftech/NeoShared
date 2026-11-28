include_guard(GLOBAL)
unset(NEO_WX_HAS_HTTPS CACHE)

option(NEO_BUILD_APPIMAGE "Build a Linux AppImage with in-app updates" OFF)

function(neo_enable_updates target application version)
    if(EMSCRIPTEN)
        return()
    endif()
    target_sources(${target} PRIVATE "${NEOSHARED_ROOT}/wx/NeoUpdater.cpp")
    target_link_libraries(${target} PRIVATE neoshared::wx neoshared::json)
    if(WIN32)
        target_link_libraries(${target} PRIVATE bcrypt)
    endif()
    target_compile_definitions(${target} PRIVATE
        NEO_UPDATE_APPLICATION="${application}"
        NEO_UPDATE_VERSION="${version}")
    if(TARGET wx::core)
        target_link_libraries(${target} PRIVATE wx::net)
    else()
        find_package(wxWidgets REQUIRED MODULE COMPONENTS net)
        target_link_libraries(${target} PRIVATE ${wxWidgets_LIBRARIES})
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        include(CheckCXXSourceCompiles)
        include(CMakePushCheckState)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_INCLUDES ${wxWidgets_INCLUDE_DIRS})
        string(JOIN " " CMAKE_REQUIRED_FLAGS ${wxWidgets_CXX_FLAGS})
        foreach(definition IN LISTS wxWidgets_DEFINITIONS)
            list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D${definition}")
        endforeach()
        if(TARGET wx::net)
            set(CMAKE_REQUIRED_LIBRARIES wx::net)
        else()
            set(CMAKE_REQUIRED_LIBRARIES ${wxWidgets_LIBRARIES})
        endif()
        check_cxx_source_compiles("#include <wx/webrequest.h>
#if !wxUSE_WEBREQUEST || !wxUSE_WEBREQUEST_CURL
#error wxWidgets requires the CURL web-request backend
#endif
int main() { auto session = wxWebSession::New(); }" NEO_WX_HAS_HTTPS)
        cmake_pop_check_state()
        if(NOT NEO_WX_HAS_HTTPS)
            message(FATAL_ERROR
                "Linux update checks require wxWidgets built with wxUSE_WEBREQUEST=ON "
                "and wxUSE_WEBREQUEST_CURL=ON. Build ${NEOSHARED_ROOT}/cmake/linux-wx, "
                "then configure with -DwxWidgets_CONFIG_EXECUTABLE=<install-prefix>/bin/wx-config. "
                "Native build commands are in ${NEOSHARED_ROOT}/README.md.")
        endif()
    endif()
endfunction()
