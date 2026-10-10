# Read-only resource indexing and lookup used by NeoBIF and NeoMDL.
add_library(neoshared_bif STATIC
    KeyBifArchive.cpp
    LooseArchiveCatalog.cpp
    ResourceQuery.cpp
    ResourceDocument.cpp)
add_library(neoshared::bif ALIAS neoshared_bif)
target_include_directories(neoshared_bif PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(neoshared_bif PUBLIC neoshared::erf)
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(neoshared_bif PUBLIC Threads::Threads)
endif()
target_compile_features(neoshared_bif PUBLIC cxx_std_17)
if(EMSCRIPTEN)
    target_compile_options(neoshared_bif PRIVATE -fexceptions)
endif()
if(MSVC)
    target_compile_options(neoshared_bif PRIVATE /W4 /permissive-)
else()
    target_compile_options(neoshared_bif PRIVATE -Wall -Wextra -Wpedantic)
endif()
