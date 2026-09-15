add_library(neoshared_texture STATIC
    Image.cpp
    FileUtil.cpp
    Txi.cpp
)
add_library(neoshared::texture ALIAS neoshared_texture)
target_include_directories(neoshared_texture PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_compile_features(neoshared_texture PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(neoshared_texture PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(neoshared_texture PRIVATE -Wall -Wextra -Wpedantic)
endif()
neo_apply_minimal_release_to_directory_targets()
