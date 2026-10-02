add_library(neoshared_model Model.cpp Geometry.cpp Lighting.cpp Emitter.cpp Trigger.cpp Wind.cpp)
add_library(neoshared::model ALIAS neoshared_model)
target_include_directories(neoshared_model PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(neoshared_model PUBLIC neoshared::path)
target_compile_features(neoshared_model PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(neoshared_model PRIVATE /W4 /permissive-)
else()
    target_compile_options(neoshared_model PRIVATE -Wall -Wextra -Wpedantic)
endif()
