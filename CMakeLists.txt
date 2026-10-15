cmake_minimum_required(VERSION 3.16)

project(neoshared
    DESCRIPTION "Shared C++17 libraries for the independent Neo tool repositories"
    LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/NeoMinimalRelease.cmake)


add_library(neoshared_path INTERFACE)
add_library(neoshared::path ALIAS neoshared_path)
target_include_directories(neoshared_path INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(neoshared_path INTERFACE cxx_std_17)

# Portable GLU arithmetic recovered from the supplied Android game library.
# GL state and game-specific upload selection remain in the consuming tool.
add_library(neoshared_glu
    glu/ByteMipmaps.cpp
    glu/Perspective.cpp
)
add_library(neoshared::glu ALIAS neoshared_glu)
target_include_directories(neoshared_glu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(neoshared_glu PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(neoshared_glu PRIVATE /W4 /permissive- /fp:strict)
else()
    target_compile_options(neoshared_glu PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
endif()

add_library(neoshared_tabular
    tabular/TabularData.cpp
)
add_library(neoshared::tabular ALIAS neoshared_tabular)
target_include_directories(neoshared_tabular PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tabular
)
target_compile_features(neoshared_tabular PUBLIC cxx_std_17)

# Reusable binary/text 2DA and GDA codec. Neo2DA retains its document/UI policy.
add_library(neoshared_twoda twoda/Common.cpp twoda/TwoDAFile.cpp)
add_library(neoshared::twoda ALIAS neoshared_twoda)
target_include_directories(neoshared_twoda PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(neoshared_twoda PUBLIC neoshared::tabular)
target_compile_features(neoshared_twoda PUBLIC cxx_std_17)

# BWM/WOK serialized geometry; game room-query policy stays in NeoMDL.
add_library(neoshared_bwm bwm/BwmFile.cpp bwm/BwmBuild.cpp)
add_library(neoshared::bwm ALIAS neoshared_bwm)
target_include_directories(neoshared_bwm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(neoshared_bwm PUBLIC cxx_std_17)

add_library(neoshared_lip lip/LipFile.cpp)
add_library(neoshared::lip ALIAS neoshared_lip)
target_include_directories(neoshared_lip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(neoshared_lip PUBLIC cxx_std_17)

add_library(neoshared_json
    json/SimpleJson.cpp
)
add_library(neoshared::json ALIAS neoshared_json)
target_include_directories(neoshared_json PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/json
)
target_compile_features(neoshared_json PUBLIC cxx_std_17)

add_library(neoshared_game
    game/Installation.cpp
)
add_library(neoshared::game ALIAS neoshared_game)
target_include_directories(neoshared_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(neoshared_game PUBLIC
    neoshared::path
    neoshared::json
)
target_compile_features(neoshared_game PUBLIC cxx_std_17)

find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    target_link_libraries(neoshared_game PRIVATE SQLite::SQLite3)
    target_compile_definitions(neoshared_game PRIVATE NEOSHARED_HAVE_SQLITE3=1)
endif()
if(WIN32)
    target_link_libraries(neoshared_game PRIVATE advapi32)
endif()

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
target_link_libraries(neoshared_gff PUBLIC neoshared::path)
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
target_link_libraries(neoshared_tlk PUBLIC neoshared::path PRIVATE neoshared::gff)
target_compile_features(neoshared_tlk PUBLIC cxx_std_17)

# Read-only dialogue semantics; all binary parsing stays in the shared cores.
add_library(neoshared_dlg dlg/Dialogue.cpp)
add_library(neoshared::dlg ALIAS neoshared_dlg)
target_include_directories(neoshared_dlg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(neoshared_dlg PUBLIC neoshared::path neoshared::gff neoshared::tlk)
target_compile_features(neoshared_dlg PUBLIC cxx_std_17)

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


option(NEOSHARED_ENABLE_ERF
    "Build the shared ERF/MOD/SAV/HAK/NWM/RIM archive core" OFF)

if(NEOSHARED_ENABLE_ERF)
    find_package(ZLIB REQUIRED)

    add_library(neoshared_erf
        erf/Archive.cpp
        erf/Utils.cpp
    )
    add_library(neoshared::erf ALIAS neoshared_erf)
    target_include_directories(neoshared_erf PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(neoshared_erf PUBLIC neoshared::path ZLIB::ZLIB)
    target_compile_features(neoshared_erf PUBLIC cxx_std_17)
    if(EMSCRIPTEN)
        target_compile_options(neoshared_erf PRIVATE -fexceptions)
    endif()
    if(MSVC)
        target_compile_options(neoshared_erf PRIVATE /W4 /permissive-)
    else()
        target_compile_options(neoshared_erf PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# KEY/BIF indexing is shared by the archive browser and model resource lookup.
if(NEOSHARED_ENABLE_ERF)
    add_subdirectory(bif EXCLUDE_FROM_ALL)
endif()

# Game textures have no wxWidgets, PNG/JPEG, or tool-repository dependency.
add_subdirectory(texture EXCLUDE_FROM_ALL)

# Typed texture properties and material bindings are shared by the tools.
add_subdirectory(material EXCLUDE_FROM_ALL)

add_library(neoshared_wx INTERFACE)
add_library(neoshared::wx ALIAS neoshared_wx)
target_include_directories(neoshared_wx INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/wx
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(neoshared_wx INTERFACE neoshared::path neoshared::game)
target_compile_features(neoshared_wx INTERFACE cxx_std_17)

neo_apply_minimal_release_to_directory_targets()
