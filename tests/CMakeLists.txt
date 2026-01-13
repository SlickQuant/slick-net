
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

add_executable(http_tests http_tests.cpp)
target_link_libraries(http_tests PRIVATE slick::net GTest::gtest_main)

# Discover tests with increased timeout
gtest_discover_tests(websocket_tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(http_tests DISCOVERY_TIMEOUT 30)

# Set test properties
set_target_properties(websocket_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

set_target_properties(http_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
