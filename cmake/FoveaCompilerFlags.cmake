# Compiler settings for Fovea targets (docs/decisions.md D-008, D-009).
#
# - Strict warnings on our own code only; third-party headers are SYSTEM
#   includes (see FoveaDependencies.cmake) so their warnings cannot fail the
#   build.
# - No fast-math, and FP contraction (FMA fusion) explicitly off, so CPU-side
#   results (ECEF math, terrain queries, replay) are repeatable for a given
#   build configuration and compiler.
# - Supported: GCC >= 11, Clang >= 14, and MSVC from Visual Studio 2022.

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(FOVEA_MSVC TRUE)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(FOVEA_MSVC FALSE)
    # clang-cl reads GCC-style warning flags differently (-Wall is -Weverything).
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(FATAL_ERROR "clang-cl is not supported; use MSVC (cl) or Clang's GNU driver")
    endif()
else()
    message(FATAL_ERROR "Fovea supports GCC, Clang and MSVC only (got ${CMAKE_CXX_COMPILER_ID})")
endif()

# C++20 library features (<numbers>, <span>, floating-point
# std::from_chars) need GCC 11, Clang 14 or MSVC 19.30 (VS 2022). RHEL 8's
# system GCC is 8.5.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
    message(FATAL_ERROR
        "Fovea needs GCC >= 11 (found ${CMAKE_CXX_COMPILER_VERSION}). On RHEL 8:\n"
        "  dnf install gcc-toolset-13-gcc-c++ && source /opt/rh/gcc-toolset-13/enable\n"
        "then configure in a fresh build directory.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "Fovea needs Clang >= 14 (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()
if(FOVEA_MSVC AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.30)
    message(FATAL_ERROR "Fovea needs MSVC from Visual Studio 2022 (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

foreach(flags_var IN ITEMS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                           CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
    if("${${flags_var}}" MATCHES "fast-math|-Ofast|finite-math-only|unsafe-math-optimizations|[/-]fp:fast")
        message(FATAL_ERROR "${flags_var} enables fast-math; forbidden by D-008")
    endif()
endforeach()

function(fovea_target_defaults target)
    if(FOVEA_MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /external:W0
            # Off by default in /W4; the nearest matches to the GCC/Clang set.
            /w14242  # conversion, possible loss of data
            /w14254  # bit-field conversion, possible loss of data
            /w14263  # member function hides a base virtual (-Woverloaded-virtual)
            /w14265  # virtual functions but non-virtual destructor (-Wnon-virtual-dtor)
            /w14287  # unsigned/negative constant mismatch
            /w14296  # expression is always true/false
            /w14365  # signed/unsigned conversion (-Wsign-conversion)
            /w14826  # sign-extending conversion
            /w15262  # implicit fall-through (-Wimplicit-fallthrough)
            /wd4324  # padding added for alignas (cache-line aligned atomics)
            # Strict IEEE semantics; since VS 2022, /fp:precise generates no
            # FMA contractions unless /fp:contract is also given.
            /fp:precise)
        # The standard C library is used as is; MSVC's non-standard "_s"
        # replacements (C4996 on sscanf, fopen, ...) are not portable.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(FOVEA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wold-style-cast
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wimplicit-fallthrough
        -fno-fast-math
        -ffp-contract=off
        # -Og keeps debugging and assertions usable while Debug builds stay
        # fast enough to run the suite; FOVEA_DEBUG_OPTIMIZATION sets it
        # (the asan-fast preset uses -O1). MSVC Debug builds keep /Od.
        $<$<CONFIG:Debug>:${FOVEA_DEBUG_OPTIMIZATION}>)
    if(FOVEA_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
