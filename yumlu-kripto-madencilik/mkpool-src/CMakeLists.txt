# mkpool - Modern Bitcoin/BCH Mining Pool
# Build (Release):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
# Requires: Linux (Ubuntu 24.04+), GCC 14+ or Clang 18+, libstdc++ with C++23.
# System deps (Ubuntu 24.04+):
#   sudo apt install -y build-essential cmake git pkg-config \
#       libboost-system-dev libboost-context-dev libssl-dev libpq-dev libpqxx-dev \
#       libzmq3-dev cppzmq-dev libsodium-dev libsecp256k1-dev zlib1g-dev
# See README.md for build, config, and run instructions.

cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(mkpool
    VERSION 0.3.0
    DESCRIPTION "Modern high-performance Bitcoin/BCH mining pool (Stratum V1, SV2-ready)"
    LANGUAGES CXX C
)

# ------------------------------------------------------------------
# Toolchain
# ------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MKPOOL_BUILD_TESTS    "Build unit tests"                                  ON)
option(MKPOOL_BUILD_FUZZERS  "Build libFuzzer targets (requires clang)"          OFF)
option(MKPOOL_ENABLE_LTO     "Enable LTO/IPO in Release"                         ON)
option(MKPOOL_ENABLE_TLS     "Enable TLS support for Stratum endpoints"          ON)
option(MKPOOL_ENABLE_METRICS "Enable Prometheus /metrics endpoint"               OFF)
option(MKPOOL_ENABLE_ASAN    "Enable AddressSanitizer (debug only - kills perf)" OFF)
option(MKPOOL_ENABLE_TSAN    "Enable ThreadSanitizer (debug only - kills perf)"  OFF)
option(MKPOOL_ENABLE_UBSAN   "Enable UndefinedBehaviorSanitizer (debug only)"    OFF)
option(MKPOOL_NATIVE         "Build with -march=native (non-portable binary)"    ON)

add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    -Wcast-align -Woverloaded-virtual -Wnull-dereference
    -Wdouble-promotion -Wformat=2
    -Wno-unused-parameter -Wno-missing-field-initializers
)

# ------------------------------------------------------------------
# Safe Release-mode optimizations + industry-standard hardening.
# All correctness-preserving (no -ffast-math, no -Ofast).
# Applied only to non-sanitizer Release builds.
# Hardening set mirrors what Debian/Ubuntu/Fedora ship by default and
# what `hardening-check` / `checksec` will look for on the binary.
# ------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release"
   AND NOT MKPOOL_ENABLE_ASAN
   AND NOT MKPOOL_ENABLE_TSAN
   AND NOT MKPOOL_ENABLE_UBSAN)
    add_compile_options(
        # --- performance (safe) ---
        -fno-math-errno                  # we never read errno from math funcs
        -fno-trapping-math               # no FP traps; allows reorder
        -fno-plt                         # remove PLT indirection (faster shared-lib calls)
        -fno-semantic-interposition      # allow inlining across TU when LTO can't see
        -falign-functions=32             # i-cache friendly on Zen/Intel
        -falign-loops=32
        -fdata-sections                  # enables --gc-sections to shrink binary
        -ffunction-sections
        # --- hardening (industry standard) ---
        -D_FORTIFY_SOURCE=3              # libc fortified wrappers (memcpy/snprintf/etc.)
        -D_GLIBCXX_ASSERTIONS            # libstdc++ bounds checks on operator[] etc.
        -fstack-protector-strong         # stack canaries on functions with arrays / addr-taken locals
        -fstack-clash-protection         # probe pages when growing stack > 4K
        -fcf-protection=full             # x86 IBT/SHSTK (CET); no-op on non-CET CPUs
        -ftrivial-auto-var-init=zero     # zero-init uninit stack vars (defeats info-leak primitives)
        -fno-delete-null-pointer-checks  # don't optimize away null-ptr UB checks
        -fno-strict-overflow             # wrap on signed overflow instead of UB-optimizing
    )
    add_link_options(
        # --- size/perf ---
        -Wl,--gc-sections                # drop unused sections (smaller binary, better TLB)
        -Wl,-O1                          # linker symbol hash optimization
        -Wl,--as-needed                  # skip unused DT_NEEDED entries
        -Wl,--hash-style=gnu             # faster dynamic symbol resolution
        # --- hardening ---
        -Wl,-z,now                       # full RELRO (resolve all symbols at load)
        -Wl,-z,relro                     # read-only relocations after load
        -Wl,-z,noexecstack               # NX stack (refuse legacy executable-stack objects)
        -Wl,-z,separate-code             # separate text/rodata segments (defeats some ROP)
        -Wl,-z,defs                      # error on unresolved symbols at link time
        -Wl,-z,relro,-z,now              # belt-and-braces full RELRO
        -pie                             # PIE binary (full ASLR for the executable too)
    )
    # PIE requires PIC on object files; CMAKE_POSITION_INDEPENDENT_CODE is already ON.
endif()

if(MKPOOL_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

if(MKPOOL_NATIVE)
    add_compile_options(-march=native)
endif()

if(MKPOOL_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(MKPOOL_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(MKPOOL_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

# ------------------------------------------------------------------
# System dependencies
# ------------------------------------------------------------------
find_package(Boost 1.83 REQUIRED CONFIG)
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX   REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(ZMQ    REQUIRED IMPORTED_TARGET libzmq>=4.3)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1)

# ------------------------------------------------------------------
# FetchContent third-party
# ------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.1.4
    GIT_SHALLOW    TRUE SYSTEM)

set(SPDLOG_FMT_EXTERNAL ON  CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL      OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE SYSTEM)

# nlohmann::json - sole JSON provider.
# Hot Stratum outbound path bypasses the DOM entirely (compile-time
# fmt::format templates in client_session.cpp + cached job-common notify
# prefix/suffix on the Stratifier). nlohmann is used only for the inbound
# parse and the cold-path RPC / config paths.
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install   OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE SYSTEM)

set(CXXOPTS_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(CXXOPTS_BUILD_TESTS    OFF CACHE INTERNAL "")
set(CXXOPTS_ENABLE_INSTALL OFF CACHE INTERNAL "")
FetchContent_Declare(cxxopts
    GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
    GIT_TAG        v3.2.1
    GIT_SHALLOW    TRUE SYSTEM)

if(MKPOOL_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE SYSTEM)
endif()

if(MKPOOL_ENABLE_METRICS)
    set(ENABLE_PUSH OFF CACHE BOOL "" FORCE)
    set(ENABLE_COMPRESSION OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(USE_THIRDPARTY_LIBRARIES ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(prometheus-cpp
        GIT_REPOSITORY https://github.com/jupp0r/prometheus-cpp.git
        GIT_TAG        v1.3.0
        GIT_SHALLOW    TRUE SYSTEM)
endif()

FetchContent_MakeAvailable(fmt spdlog nlohmann_json cxxopts)
if(MKPOOL_BUILD_TESTS)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()
if(MKPOOL_ENABLE_METRICS)
    FetchContent_MakeAvailable(prometheus-cpp)
endif()

# ------------------------------------------------------------------
# Core static library (so tests can link without main)
# ------------------------------------------------------------------
set(MKPOOL_CORE_SOURCES
    src/address.cpp
    src/merkle.cpp
    src/vardiff.cpp
    src/rate_limiter.cpp
    src/db_worker.cpp
    src/io_pool.cpp
    src/metrics.cpp
    src/tls_context.cpp
    src/stratum_protocol.cpp
    src/stratifier.cpp
    src/client_session.cpp
    src/connector.cpp
    src/generator.cpp
    src/pool_manager.cpp
    src/bitcoin_client.cpp
    src/beast_client.cpp
    src/zmq_client.cpp
    src/database.cpp
    src/network_stats.cpp
    src/sv2_noise.cpp
    src/blacklist.cpp
    src/control_server.cpp
    src/relay_session.cpp
    src/relay_listener.cpp
    src/upstream_client.cpp
    src/proxy_session.cpp
    src/redirector_session.cpp
    src/cluster_ingest.cpp
    src/cluster_trunk.cpp
    src/cluster_block_relay.cpp
    src/sv2_relay_session.cpp
    src/sv2_upstream_client.cpp
)

# ------------------------------------------------------------------
# Executables must export nothing.
#
# libpqxx's type_name<T> is a non-inline variable template, so every TU touching
# a pqxx conversion instantiates it. GCC 15 puts those instantiations in the
# executable's dynamic symbol table, where they preempt libpqxx.so's identical
# definitions -- both DSOs then construct AND free the same std::string, aborting
# at exit with "double free detected in tcache 2" inside __cxa_finalize.
# GCC 14 happens not to export them, which is the only reason this has not bitten
# production yet. See cmake/hidden-exports.map for the full analysis.
#
# INTERFACE library so every executable picks it up by linking it.
# ------------------------------------------------------------------
add_library(mkpool_hidden_exports INTERFACE)
if(UNIX AND NOT APPLE)
    set(MKPOOL_EXPORTS_MAP ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hidden-exports.map)
    target_link_options(mkpool_hidden_exports INTERFACE
        "-Wl,--version-script=${MKPOOL_EXPORTS_MAP}")
    # Relink when the script changes.
    set_property(TARGET mkpool_hidden_exports APPEND PROPERTY
        INTERFACE_LINK_DEPENDS ${MKPOOL_EXPORTS_MAP})
endif()

add_library(mkpool_core STATIC ${MKPOOL_CORE_SOURCES})
target_include_directories(mkpool_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(mkpool_core PUBLIC cxx_std_23)
target_precompile_headers(mkpool_core PRIVATE src/pch.hpp)

target_link_libraries(mkpool_core
    PUBLIC
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        fmt::fmt
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        cxxopts::cxxopts
        PkgConfig::PQXX
        PostgreSQL::PostgreSQL
        PkgConfig::ZMQ
        PkgConfig::SODIUM
        PkgConfig::SECP256K1
        ${CMAKE_DL_LIBS}
)

if(MKPOOL_ENABLE_METRICS)
    target_link_libraries(mkpool_core PUBLIC prometheus-cpp::pull)
    target_compile_definitions(mkpool_core PUBLIC MKPOOL_HAS_METRICS=1)
endif()
if(MKPOOL_ENABLE_TLS)
    target_compile_definitions(mkpool_core PUBLIC MKPOOL_HAS_TLS=1)
endif()

# ------------------------------------------------------------------
# Executable
# ------------------------------------------------------------------
add_executable(mkpool src/mkpool.cpp)
target_link_libraries(mkpool PRIVATE mkpool_core mkpool_hidden_exports)

# Stage a runnable config in the build dir so ./build/mkpool works out of the
# box. Prefer a real config.json if the operator has one (it is gitignored and
# kept local); otherwise fall back to the shipped example with placeholders.
if(EXISTS "${CMAKE_SOURCE_DIR}/config.json")
    set(CONFIG_SOURCE "${CMAKE_SOURCE_DIR}/config.json")
else()
    set(CONFIG_SOURCE "${CMAKE_SOURCE_DIR}/config.json.example")
endif()
set(CONFIG_DEST   "${CMAKE_BINARY_DIR}/config.json")
add_custom_command(TARGET mkpool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CONFIG_SOURCE}" "${CONFIG_DEST}"
    BYPRODUCTS "${CONFIG_DEST}"
    COMMENT "Staging config.json in build dir")

# ------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------
if(MKPOOL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ------------------------------------------------------------------
# Fuzzers (clang only; independent of MKPOOL_BUILD_TESTS)
# ------------------------------------------------------------------
if(MKPOOL_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

# ------------------------------------------------------------------
# Install
# ------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS mkpool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config.json.example DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/mkpool)
