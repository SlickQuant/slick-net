cmake_minimum_required(VERSION 3.20)

if (MSVC)
    option(LINK_STATICALLY "Build tests" OFF)
    if (LINK_STATICALLY)
        set(VCPKG_TARGET_TRIPLET x64-windows-static)
    endif()
endif()

project(slick-net
    VERSION 1.2.1
    LANGUAGES CXX
)

message(STATUS "slick-net: ${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (LINK_STATICALLY)
    set(Boost_USE_STATIC_LIBS ON)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    set(OPENSSL_MSVC_STATIC_RT TRUE)
endif()
find_package(Boost CONFIG REQUIRED COMPONENTS beast context)
find_package(OpenSSL REQUIRED)

include(FetchContent)

# Disable tests for slick-queue
set(BUILD_SLICK_QUEUE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    slick-queue
    GIT_REPOSITORY https://github.com/SlickQuant/slick-queue.git
    GIT_TAG v1.2.2
)
FetchContent_MakeAvailable(slick-queue)

message(STATUS "OpenSSL: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "Beast: ${boost_beast_CONFIG}")

# Add header-only library
add_library(slick-net INTERFACE)
add_library(slick::net ALIAS slick-net)
# For backward compatiblity
add_library(slick_net ALIAS slick-net)
add_library(slick::slick_net ALIAS slick-net)

target_include_directories(slick-net INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(slick-net INTERFACE slick::queue Boost::asio Boost::beast Boost::context OpenSSL::SSL OpenSSL::Crypto)
target_precompile_headers(slick-net INTERFACE
    # Standard library headers
    <atomic>
    <cstdlib>
    <functional>
    <unordered_map>
    <mutex>
    <csignal>
    <thread>
    # Boost headers
    <boost/beast/core.hpp>
    <boost/beast/websocket.hpp>
    <boost/beast/websocket/ssl.hpp>
    <boost/beast/http.hpp>
    <boost/asio/ssl.hpp>
    <boost/asio/strand.hpp>
    <boost/asio/awaitable.hpp>
    <boost/asio/co_spawn.hpp>
    <boost/asio/signal_set.hpp>
    <boost/asio/ip/tcp.hpp>
    <boost/asio/ssl/stream.hpp>
    <boost/asio/connect.hpp>
    <boost/asio/post.hpp>
    <boost/asio/as_tuple.hpp>
    # External dependencies
    <slick/queue.h>
)
set_target_properties(slick-net PROPERTIES EXPORT_NAME net)

if (MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")
    add_definitions(-D_WIN32_WINNT=0x0A00)
    set(CMAKE_SUPPRESS_REGENERATION true)   # supress zero_check
    set_target_properties(slick-net PROPERTIES LINK_INCREMENTAL ON)
    target_compile_options(slick-net INTERFACE "/bigobj")
    if (LINK_STATICALLY)
        set_target_properties(slick-net PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")
endif()

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
  if(MSVC)
    # MSVC AddressSanitizer (requires Visual Studio 2022 17.7+)
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.37")
      add_compile_options(/fsanitize=address)
      add_link_options(/INCREMENTAL:NO)

      # Additional MSVC ASan flags for better compatibility
      add_compile_definitions(_DISABLE_VECTOR_ANNOTATION)
      add_compile_definitions(_DISABLE_STRING_ANNOTATION)

      message(STATUS "AddressSanitizer enabled for MSVC ${CMAKE_CXX_COMPILER_VERSION}")
    else()
      message(WARNING "AddressSanitizer requires Visual Studio 2022 17.7+ (current: ${CMAKE_CXX_COMPILER_VERSION})")
      set(ENABLE_ASAN OFF CACHE BOOL "ASan disabled - compiler too old" FORCE)
    endif()
  else()
    # GCC/Clang AddressSanitizer
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

option(BUILD_SLICK_NET_TESTS "Build tests" ON)
if (BUILD_SLICK_NET_TESTS)
    message(STATUS "Building slick-net tests")
    enable_testing()
    add_subdirectory(tests)
else()
    message(STATUS "Skipping slick-net tests")
endif()

option(BUILD_SLICK_NET_EXAMPLES "Build examples" ON)
if(BUILD_SLICK_NET_EXAMPLES)
    message(STATUS "Building slick::net examples")
    add_subdirectory(examples)
else()
    message(STATUS "Skipping slick::net examples")
endif()

# Installation rules
install(DIRECTORY include/ DESTINATION include)

# Install targets and create CMake config files
install(TARGETS slick-net
    EXPORT slick-net-targets
    INCLUDES DESTINATION include
)

install(EXPORT slick-net-targets
    FILE slick-net-targets.cmake
    NAMESPACE slick::
    DESTINATION share/slick-net
)

# Create config file
include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/slick-net-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/slick-net-config.cmake"
    INSTALL_DESTINATION share/slick-net
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/slick-net-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/slick-net-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/slick-net-config-version.cmake"
    DESTINATION share/slick-net
)