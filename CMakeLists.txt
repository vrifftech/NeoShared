cmake_minimum_required(VERSION 3.16)

project(neoshared
    DESCRIPTION "Shared C++17 libraries for the independent Neo tool repositories"
    LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/NeoMinimalRelease.cmake)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_NEOSHARED_TESTS_DEFAULT ON)
else()
    set(_NEOSHARED_TESTS_DEFAULT OFF)
endif()
option(NEOSHARED_BUILD_TESTS "Build neoshared regression tests" ${_NEOSHARED_TESTS_DEFAULT})
unset(_NEOSHARED_TESTS_DEFAULT)

add_library(neoshared_tabular
    tabular/TabularData.cpp
)
add_library(neoshared::tabular ALIAS neoshared_tabular)
target_include_directories(neoshared_tabular PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tabular
)
target_compile_features(neoshared_tabular PUBLIC cxx_std_17)

add_library(neoshared_json
    json/SimpleJson.cpp
)
add_library(neoshared::json ALIAS neoshared_json)
target_include_directories(neoshared_json PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/json
)
target_compile_features(neoshared_json PUBLIC cxx_std_17)

add_library(neoshared_xml
    xml/SimpleXml.cpp
)
add_library(neoshared::xml ALIAS neoshared_xml)
target_include_directories(neoshared_xml PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/xml
)
target_compile_features(neoshared_xml PUBLIC cxx_std_17)

add_library(neoshared_tslpatcher
    tslpatcher/TslPatcher.cpp
)
add_library(neoshared::tslpatcher ALIAS neoshared_tslpatcher)
target_include_directories(neoshared_tslpatcher PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tslpatcher
)
target_link_libraries(neoshared_tslpatcher PUBLIC neoshared::tabular)
target_compile_features(neoshared_tslpatcher PUBLIC cxx_std_17)

add_library(neoshared_gff
    gff/Common.cpp
    gff/GFFFile.cpp
    gff/GffXml.cpp
    gff/GffJson.cpp
    gff/GffTypeNames.cpp
)
add_library(neoshared::gff ALIAS neoshared_gff)
target_include_directories(neoshared_gff PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/gff
)
target_link_libraries(neoshared_gff PUBLIC
    neoshared::tabular
    neoshared::json
    neoshared::xml
)
target_compile_features(neoshared_gff PUBLIC cxx_std_17)

add_library(neoshared_tlk
    tlk/TlkFile.cpp
    tlk/TextEncoding.cpp
    tlk/TlkLookup.cpp
)
add_library(neoshared::tlk ALIAS neoshared_tlk)
target_include_directories(neoshared_tlk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tlk/include
)
target_link_libraries(neoshared_tlk PRIVATE neoshared::gff)
target_compile_features(neoshared_tlk PUBLIC cxx_std_17)

add_library(neoshared_gff_app
    gff/AppModel.cpp
)
add_library(neoshared::gff_app ALIAS neoshared_gff_app)
target_include_directories(neoshared_gff_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/gff
)
target_link_libraries(neoshared_gff_app PUBLIC
    neoshared::gff
    neoshared::tlk
)
target_compile_features(neoshared_gff_app PUBLIC cxx_std_17)

add_library(neoshared_wx INTERFACE)
add_library(neoshared::wx ALIAS neoshared_wx)
target_include_directories(neoshared_wx INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/wx
)
target_compile_features(neoshared_wx INTERFACE cxx_std_17)

if(NEOSHARED_BUILD_TESTS)
    enable_testing()
    add_executable(neoshared-format-tests tests/format_tests.cpp)
    target_link_libraries(neoshared-format-tests PRIVATE neoshared::gff neoshared::tlk)
    target_compile_features(neoshared-format-tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(neoshared-format-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(neoshared-format-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME neoshared-formats COMMAND neoshared-format-tests)
endif()

neo_apply_minimal_release_to_directory_targets()
