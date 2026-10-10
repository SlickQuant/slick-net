# A stand-alone project that consumes an *installed* slick-net exactly as the README tells users
# to. The package_consumer test in ../CMakeLists.txt installs the build tree to a scratch prefix
# and builds this against it; it can also be configured by hand against any installed slick-net.
cmake_minimum_required(VERSION 3.21)
project(slick_net_package_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(slick-net CONFIG REQUIRED)
message(STATUS "Found slick-net ${slick-net_VERSION}: ${slick-net_DIR}")

# Set by the package_consumer test, absent in a hand-configured build
if (DEFINED SLICK_NET_EXPECTED_VERSION AND NOT slick-net_VERSION VERSION_EQUAL SLICK_NET_EXPECTED_VERSION)
    message(FATAL_ERROR
        "Found slick-net ${slick-net_VERSION} in ${slick-net_DIR}, expected ${SLICK_NET_EXPECTED_VERSION}")
endif()

# Regression: the installed config once kept requiring slick-stream-buffer-multiplexer 2.0.0 after
# the build had moved to 2.1.0, so an installed slick-net accepted a dependency its own build
# rejected. The installed config must demand exactly the floors the build did.
file(READ "${slick-net_DIR}/slick-net-config.cmake" installed_config)
foreach(dependency IN ITEMS slick-stream-buffer-multiplexer slick-dynamic-buffer)
    if (DEFINED SLICK_NET_EXPECTED_${dependency}_VERSION)
        set(expected "find_dependency(${dependency} ${SLICK_NET_EXPECTED_${dependency}_VERSION} CONFIG)")
        string(FIND "${installed_config}" "${expected}" position)
        if (position EQUAL -1)
            message(FATAL_ERROR
                "${slick-net_DIR}/slick-net-config.cmake does not contain '${expected}': "
                "the installed package's dependency floor differs from the one slick-net was built with")
        endif()
    endif()
endforeach()

add_executable(slick_net_package_consumer main.cpp)
target_link_libraries(slick_net_package_consumer PRIVATE slick::net)
if (MSVC)
    target_compile_options(slick_net_package_consumer PRIVATE "/bigobj")
endif()

enable_testing()
add_test(NAME package_consumer COMMAND slick_net_package_consumer)
