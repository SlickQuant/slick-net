
find_package(GTest CONFIG QUIET)

if (GTest_FOUND)
    message(STATUS "GTest found: ${GTest_VERSION}")
else()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
    )
    if (WIN32)
        # For Windows: Prevent overriding the parent project's compiler/linker on Windows
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(websocket_tests websocket_tests.cpp)
target_link_libraries(websocket_tests PRIVATE slick::net GTest::gtest_main)
if (MSVC)
    target_compile_options(websocket_tests PRIVATE "/bigobj")
endif()

add_executable(websocket_service_tests websocket_service_tests.cpp)
target_link_libraries(websocket_service_tests PRIVATE slick::net GTest::gtest_main)

add_executable(http_tests http_tests.cpp)
target_link_libraries(http_tests PRIVATE slick::net GTest::gtest_main)

add_executable(tls_tests tls_tests.cpp)
target_link_libraries(tls_tests PRIVATE slick::net GTest::gtest_main)
if (MSVC)
    target_compile_options(tls_tests PRIVATE "/bigobj")
endif()

add_executable(logging_tests logging_tests.cpp)
target_link_libraries(logging_tests PRIVATE slick::net GTest::gtest_main)

add_executable(signal_tests signal_tests.cpp)
target_link_libraries(signal_tests PRIVATE slick::net GTest::gtest_main)

add_executable(write_chain_gate_tests write_chain_gate_tests.cpp)
target_link_libraries(write_chain_gate_tests PRIVATE slick::net GTest::gtest_main)

add_executable(slick_buffer_tests slick_buffer_websocket_tests.cpp)
target_link_libraries(slick_buffer_tests PRIVATE slick::net GTest::gtest_main)
target_include_directories(slick_buffer_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
# CMake's stock optimized configs all define NDEBUG; the test asserts the build flags kept it
target_compile_definitions(slick_buffer_tests PRIVATE
    SLICK_NET_TEST_EXPECT_NDEBUG=$<CONFIG:Release,RelWithDebInfo,MinSizeRel>)
if (MSVC)
    target_compile_options(slick_buffer_tests PRIVATE "/bigobj")
endif()

# ── test discovery ───────────────────────────────────────────────────────────
# Discover tests at ctest time (PRE_TEST) instead of build time so a slow
# first launch of a freshly linked exe (AV scan, DLL load) can't fail the build.
# A per-test TIMEOUT turns a deadlock into a failure instead of hanging the CI job.
gtest_discover_tests(websocket_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
gtest_discover_tests(websocket_service_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
gtest_discover_tests(http_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
gtest_discover_tests(tls_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
gtest_discover_tests(logging_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
gtest_discover_tests(signal_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
gtest_discover_tests(slick_buffer_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
# The write-chain race tests spin for a fixed budget of their own, so they need more than the rest
gtest_discover_tests(write_chain_gate_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 180)

# Set test properties
set_target_properties(websocket_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

set_target_properties(websocket_service_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

set_target_properties(http_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

set_target_properties(tls_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

set_target_properties(logging_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

set_target_properties(signal_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

set_target_properties(slick_buffer_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

set_target_properties(write_chain_gate_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

# ── installed-package consumer ───────────────────────────────────────────────
# Installs this build tree to a scratch prefix, then configures, builds and runs
# tests/package_consumer against it with nothing but find_package(slick-net). Nothing else
# exercises the installed package: its config file, export set and dependency floors.
# ASan builds are skipped - the consumer would have to repeat the sanitizer flags.
if (NOT ENABLE_ASAN)
    set(package_consumer_dir ${CMAKE_BINARY_DIR}/package_consumer)
    set(package_consumer_prefix ${package_consumer_dir}/prefix)

    # The consumer's initial cache: the toolchain this build used, slick-net pinned to the
    # scratch prefix, and that prefix on the search path - dependencies this build fetched are
    # installed into it too, and are found there when no installed package provides them.
    #
    # Regression: values were written as quoted strings, so a path given to this build with
    # backslashes (-DCMAKE_PREFIX_PATH=D:\a\... on a Windows CI runner) made the script a syntax
    # error - "Invalid character escape '\a'". Every value is now a bracket argument, which has
    # no escape sequences, and paths are normalized to forward slashes first.
    set(package_consumer_cache "")
    function(package_consumer_cache_set var value)
        string(APPEND package_consumer_cache "set(${var} [==[${value}]==] CACHE STRING [==[]==])\n")
        set(package_consumer_cache "${package_consumer_cache}" PARENT_SCOPE)
    endfunction()
    function(package_consumer_cache_set_paths var)
        set(paths "")
        foreach(path IN LISTS ARGN)
            cmake_path(SET path NORMALIZE "${path}")
            list(APPEND paths "${path}")
        endforeach()
        package_consumer_cache_set(${var} "${paths}")
        set(package_consumer_cache "${package_consumer_cache}" PARENT_SCOPE)
    endfunction()

    package_consumer_cache_set(slick-net_DIR "${package_consumer_prefix}/share/slick-net")
    package_consumer_cache_set_paths(CMAKE_PREFIX_PATH "${package_consumer_prefix}" $CACHE{CMAKE_PREFIX_PATH})
    package_consumer_cache_set(SLICK_NET_EXPECTED_VERSION "${PROJECT_VERSION}")
    package_consumer_cache_set(SLICK_NET_EXPECTED_slick-stream-buffer-multiplexer_VERSION
        "${SLICK_NET_STREAM_BUFFER_MULTIPLEXER_MIN_VERSION}")
    package_consumer_cache_set(SLICK_NET_EXPECTED_slick-dynamic-buffer_VERSION
        "${SLICK_NET_DYNAMIC_BUFFER_MIN_VERSION}")

    set(package_consumer_forwarded_paths CMAKE_TOOLCHAIN_FILE CMAKE_OSX_SYSROOT)
    if (NOT CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
        # IDE generators pick the compiler and build tool themselves
        list(APPEND package_consumer_forwarded_paths CMAKE_CXX_COMPILER CMAKE_MAKE_PROGRAM)
    endif()
    foreach(var IN LISTS package_consumer_forwarded_paths)
        if (DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
            package_consumer_cache_set_paths(${var} "${${var}}")
        endif()
    endforeach()
    foreach(var IN ITEMS VCPKG_TARGET_TRIPLET CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET
                         Boost_USE_STATIC_LIBS OPENSSL_USE_STATIC_LIBS OPENSSL_MSVC_STATIC_RT)
        if (DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
            package_consumer_cache_set(${var} "${${var}}")
        endif()
    endforeach()
    if (MSVC AND LINK_STATICALLY)
        package_consumer_cache_set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    file(WRITE ${package_consumer_dir}/initial_cache.cmake "${package_consumer_cache}")

    # Only the configuration under test gets installed, so that is the only one the consumer
    # may generate: an imported slick::net has no library for the others.
    get_property(package_consumer_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if (package_consumer_multi_config)
        set(package_consumer_config -DCMAKE_CONFIGURATION_TYPES=$<CONFIG>)
    else()
        set(package_consumer_config -DCMAKE_BUILD_TYPE=$<CONFIG>)
    endif()

    set(package_consumer_generator --build-generator ${CMAKE_GENERATOR})
    if (CMAKE_GENERATOR_PLATFORM)
        list(APPEND package_consumer_generator --build-generator-platform ${CMAKE_GENERATOR_PLATFORM})
    endif()
    if (CMAKE_GENERATOR_TOOLSET)
        list(APPEND package_consumer_generator --build-generator-toolset ${CMAKE_GENERATOR_TOOLSET})
    endif()

    # A prefix left by an earlier run could satisfy the consumer with a file this build no
    # longer installs
    add_test(NAME package_consumer_clean
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${package_consumer_prefix})
    add_test(NAME package_consumer_install
        COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --prefix ${package_consumer_prefix})
    add_test(NAME package_consumer
        COMMAND ${CMAKE_CTEST_COMMAND}
            --build-and-test ${CMAKE_CURRENT_SOURCE_DIR}/package_consumer ${package_consumer_dir}/build
            ${package_consumer_generator}
            --build-config $<CONFIG>
            --build-options -C ${package_consumer_dir}/initial_cache.cmake ${package_consumer_config}
            --test-command ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure)

    set_tests_properties(package_consumer_clean PROPERTIES FIXTURES_SETUP package_consumer_prefix)
    set_tests_properties(package_consumer_install PROPERTIES
        FIXTURES_SETUP package_consumer_prefix DEPENDS package_consumer_clean TIMEOUT 300)
    # Configures and compiles a Boost.Beast translation unit from scratch
    set_tests_properties(package_consumer PROPERTIES FIXTURES_REQUIRED package_consumer_prefix TIMEOUT 900)
endif()
