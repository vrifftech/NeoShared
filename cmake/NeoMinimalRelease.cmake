include_guard(GLOBAL)

# Release-minimization helpers shared by the independent Neo repositories.
#
# Enable with -DNEO_MINIMAL_RELEASE=ON. This keeps release builds from
# carrying debug/STABS sections, absolute source/build paths, or avoidable
# symbol metadata.

option(NEO_MINIMAL_RELEASE "Build release executables without debug symbols, STABS, or absolute source/build paths" OFF)
option(NEO_MINIMAL_REMAP_PATHS "Remap source/build paths in compiler-emitted strings and debug-prefix metadata" ON)
option(NEO_MINIMAL_GC_SECTIONS "Discard unused functions/data in non-Debug builds when supported" ON)
option(NEO_MINIMAL_NO_BUILD_ID "Disable ELF build-id notes when the linker supports it" ON)

get_filename_component(_NEO_MINIMAL_SHARED_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
get_filename_component(_NEO_MINIMAL_PROJECT_ROOT "${CMAKE_SOURCE_DIR}" REALPATH)
get_filename_component(_NEO_MINIMAL_BINARY_ROOT "${CMAKE_BINARY_DIR}" REALPATH)

function(neo_apply_minimal_release target_name)
    if(NOT NEO_MINIMAL_RELEASE OR NOT TARGET ${target_name})
        return()
    endif()

    get_target_property(_neo_target_type ${target_name} TYPE)
    if(_neo_target_type STREQUAL "INTERFACE_LIBRARY" OR _neo_target_type STREQUAL "UTILITY")
        return()
    endif()

    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:/Brepro>
        )
        if(_neo_target_type STREQUAL "EXECUTABLE" OR _neo_target_type STREQUAL "SHARED_LIBRARY" OR _neo_target_type STREQUAL "MODULE_LIBRARY")
            target_link_options(${target_name} PRIVATE
                $<$<NOT:$<CONFIG:Debug>>:/DEBUG:NONE>
                $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>
                $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
                $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
                $<$<NOT:$<CONFIG:Debug>>:/Brepro>
            )
        endif()
    else()
        target_compile_options(${target_name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:-g0>
        )

        if(NEO_MINIMAL_REMAP_PATHS)
            foreach(_neo_path IN ITEMS
                "${_NEO_MINIMAL_SHARED_ROOT}"
                "${_NEO_MINIMAL_PROJECT_ROOT}"
                "${_NEO_MINIMAL_BINARY_ROOT}")
                if(_neo_path)
                    target_compile_options(${target_name} PRIVATE
                        $<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${_neo_path}=.>
                        $<$<NOT:$<CONFIG:Debug>>:-fdebug-prefix-map=${_neo_path}=.>
                        $<$<NOT:$<CONFIG:Debug>>:-fmacro-prefix-map=${_neo_path}=.>
                    )
                endif()
            endforeach()
        endif()

        if(NEO_MINIMAL_GC_SECTIONS)
            target_compile_options(${target_name} PRIVATE
                $<$<NOT:$<CONFIG:Debug>>:-ffunction-sections>
                $<$<NOT:$<CONFIG:Debug>>:-fdata-sections>
            )
        endif()

        if(_neo_target_type STREQUAL "EXECUTABLE" OR _neo_target_type STREQUAL "SHARED_LIBRARY" OR _neo_target_type STREQUAL "MODULE_LIBRARY")
            if(APPLE)
                if(NEO_MINIMAL_GC_SECTIONS)
                    target_link_options(${target_name} PRIVATE
                        $<$<NOT:$<CONFIG:Debug>>:-Wl,-dead_strip>
                    )
                endif()
            else()
                if(NEO_MINIMAL_GC_SECTIONS)
                    target_link_options(${target_name} PRIVATE
                        $<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>
                    )
                endif()
                target_link_options(${target_name} PRIVATE
                    $<$<NOT:$<CONFIG:Debug>>:-Wl,--strip-all>
                )
                if(NEO_MINIMAL_NO_BUILD_ID)
                    target_link_options(${target_name} PRIVATE
                        $<$<NOT:$<CONFIG:Debug>>:-Wl,--build-id=none>
                    )
                endif()
            endif()
        endif()
    endif()
endfunction()

function(neo_apply_minimal_release_to_directory_targets)
    if(NOT NEO_MINIMAL_RELEASE)
        return()
    endif()
    get_property(_neo_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_neo_target IN LISTS _neo_targets)
        neo_apply_minimal_release(${_neo_target})
    endforeach()
endfunction()
