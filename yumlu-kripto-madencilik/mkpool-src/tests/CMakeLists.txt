# SPDX-License-Identifier: GPL-3.0
# Unit tests for mkpool core modules.

include(Catch)

add_executable(mkpool_tests
    test_main.cpp
    test_address.cpp
    test_merkle.cpp
    test_vardiff.cpp
    test_rate_limiter.cpp
    test_job_window.cpp
    test_stratum_configure.cpp
    test_write_queue.cpp
    test_sv2_noise.cpp
    test_sv2_framing.cpp
    test_stratum_v1_hostile.cpp
    test_concurrency_stress.cpp
)

target_link_libraries(mkpool_tests
    PRIVATE
        mkpool_core
        mkpool_hidden_exports   # same libpqxx/type_name reason as the mkpool binary
        Catch2::Catch2WithMain
)

target_include_directories(mkpool_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

# DISCOVERY_MODE PRE_TEST, not the POST_BUILD default.
#
# POST_BUILD runs the freshly linked binary during `cmake --build` to enumerate
# test cases. If the binary cannot start there -- sanitizer runtime vs the host's
# ASLR settings, a missing runtime lib, anything -- the BUILD fails inside
# CatchAddTests.cmake, with the real cause buried in a CMake error and every job
# broken at once, including ones that never intended to run tests.
#
# PRE_TEST defers enumeration to ctest. The build then only builds, and a binary
# that will not start surfaces when tests run, where the error is legible and
# scoped to the test step.
catch_discover_tests(mkpool_tests DISCOVERY_MODE PRE_TEST)
