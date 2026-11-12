# Build upstream Opus sources directly from this tracked component directory.
# Keep the source lists in sync with the upstream *.mk files.
set(OPUS_ROOT "${CMAKE_CURRENT_LIST_DIR}")

function(opus_collect_sources variable makefile output)
    file(STRINGS "${OPUS_ROOT}/${makefile}" lines)
    set(found_variable FALSE)
    set(sources "")

    foreach(line IN LISTS lines)
        if(NOT found_variable)
            if(line MATCHES "^${variable}[ \t]*=")
                set(found_variable TRUE)
                string(REGEX REPLACE "^[^=]*=[ \t]*" "" line "${line}")
            else()
                continue()
            endif()
        elseif(line STREQUAL "" OR line MATCHES "^[A-Za-z0-9_]+[ \t]*=")
            break()
        endif()

        string(REGEX REPLACE "[ \t]*\\\\$" "" line "${line}")
        string(STRIP "${line}" line)
        if(NOT line STREQUAL "")
            list(APPEND sources "${line}")
        endif()
    endforeach()

    if(NOT found_variable OR NOT sources)
        message(FATAL_ERROR "Could not read ${variable} from ${OPUS_ROOT}/${makefile}")
    endif()
    list(TRANSFORM sources STRIP)
    list(REMOVE_ITEM sources "")
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()

opus_collect_sources(OPUS_SOURCES opus_sources.mk opus_core_sources)
opus_collect_sources(OPUS_SOURCES_FLOAT opus_sources.mk opus_analysis_sources)
opus_collect_sources(CELT_SOURCES celt_sources.mk opus_celt_sources)
opus_collect_sources(SILK_SOURCES silk_sources.mk opus_silk_sources)
opus_collect_sources(SILK_SOURCES_FIXED silk_sources.mk opus_silk_fixed_sources)

set(OPUS_COMPONENT_SOURCES
    ${opus_core_sources}
    ${opus_analysis_sources}
    ${opus_celt_sources}
    ${opus_silk_sources}
    ${opus_silk_fixed_sources}
)

idf_component_register(
    SRCS ${OPUS_COMPONENT_SOURCES}
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    PRIV_INCLUDE_DIRS
        "src"
        "celt"
        "silk"
        "silk/fixed"
)

# During ESP-IDF dependency scanning COMPONENT_LIB is an INTERFACE library.
# Apply implementation-only definitions during the real build pass.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        OPUS_BUILD
        OPUS_XTENSA_LX7
        FIXED_POINT=1
        NONTHREADSAFE_PSEUDOSTACK
        OPUS_ESP32_PER_CORE_STACK
        GLOBAL_STACK_SIZE=60000
        HAVE_LRINT
        HAVE_LRINTF
        OPUS_BENCH_STAGE_PROFILE
    )
endif()
