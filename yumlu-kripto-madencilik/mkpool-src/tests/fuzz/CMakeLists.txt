# SPDX-License-Identifier: GPL-3.0
# libFuzzer targets for the unauthenticated wire surface (Stratum V1 + V2).
#
# Requires clang (libFuzzer is not available under GCC):
#   cmake -S . -B build-fuzz -DMKPOOL_BUILD_FUZZERS=ON -DMKPOOL_BUILD_TESTS=OFF \
#         -DMKPOOL_NATIVE=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build-fuzz -j
#   ./build-fuzz/tests/fuzz/fuzz_sv2_frame tests/fuzz/corpus -dict=tests/fuzz/sv2.dict

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "MKPOOL_BUILD_FUZZERS=ON requires clang (libFuzzer). "
        "Re-configure with -DCMAKE_CXX_COMPILER=clang++")
endif()

# fuzzer    -> libFuzzer driver + coverage instrumentation
# address   -> the heap-overflow class that caused the 2026-07-15 outage
# undefined -> the integer/shift UB that precedes most length bugs
set(MKPOOL_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)

function(mkpool_add_fuzzer target)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})

    add_executable(${target} ${target}.cpp ${ARG_SOURCES})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE ${MKPOOL_FUZZ_FLAGS})
    target_link_options(${target} PRIVATE ${MKPOOL_FUZZ_FLAGS})
    if(ARG_LIBS)
        target_link_libraries(${target} PRIVATE ${ARG_LIBS})
    endif()

    # UBSan only warns by default; make it fail so CI actually gates on it.
    set_property(TARGET ${target} PROPERTY
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endfunction()

# Group 1: Stratum V2 -- header-only, no third-party deps. CI compiles these
# directly with clang++ rather than through CMake to keep that property.
mkpool_add_fuzzer(fuzz_sv2_frame)
mkpool_add_fuzzer(fuzz_sv2_message)
mkpool_add_fuzzer(fuzz_sv2_codec)

# Group 2: needs real translation units (each includes pch.hpp, which drags in
# fmt/spdlog/nlohmann/OpenSSL/Boost/pqxx). Sources are compiled INTO the target
# rather than linked from mkpool_core so they carry coverage instrumentation --
# linking an uninstrumented library leaves the fuzzer blind.
mkpool_add_fuzzer(fuzz_address
    SOURCES ${CMAKE_SOURCE_DIR}/src/address.cpp
    LIBS    OpenSSL::Crypto
            fmt::fmt
            spdlog::spdlog
            nlohmann_json::nlohmann_json
            Boost::headers
            PkgConfig::PQXX)

mkpool_add_fuzzer(fuzz_stratum_configure
    SOURCES ${CMAKE_SOURCE_DIR}/src/stratum_protocol.cpp
    LIBS    OpenSSL::Crypto
            fmt::fmt
            spdlog::spdlog
            nlohmann_json::nlohmann_json
            Boost::headers
            PkgConfig::PQXX)

# The Noise handshake/transport: the first code an unauthenticated peer reaches
# on an SV2 port, ahead of every other guard.
mkpool_add_fuzzer(fuzz_sv2_noise
    SOURCES ${CMAKE_SOURCE_DIR}/src/sv2_noise.cpp
    LIBS    OpenSSL::Crypto
            PkgConfig::SODIUM
            PkgConfig::SECP256K1
            fmt::fmt
            spdlog::spdlog
            nlohmann_json::nlohmann_json
            Boost::headers
            PkgConfig::PQXX)
