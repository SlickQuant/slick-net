cmake_minimum_required(VERSION 3.21)

option(LINK_STATICALLY "Link Boost and OpenSSL statically" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(BUILD_SLICK_NET_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(BUILD_SLICK_NET_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})

if (LINK_STATICALLY)
    if (MSVC)
        set(VCPKG_TARGET_TRIPLET x64-windows-static)
    elseif (APPLE)
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|ARM64")
            set(VCPKG_TARGET_TRIPLET arm64-osx-static)
        else()
            set(VCPKG_TARGET_TRIPLET x64-osx-static)
        endif()
    endif()
    # Linux x64-linux is already static by default
endif()

project(slick-net
    VERSION 3.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (LINK_STATICALLY)
    set(Boost_USE_STATIC_LIBS ON)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    if (MSVC)
        set(OPENSSL_MSVC_STATIC_RT TRUE)
    endif()
endif()
find_package(Boost CONFIG REQUIRED COMPONENTS beast context)
find_package(OpenSSL CONFIG REQUIRED)

find_package(slick-shm CONFIG QUIET)
if (slick-shm_FOUND)
    message(STATUS "slick-shm: ${slick-shm_VERSION}")
endif()

find_package(slick-queue 1.5.0 CONFIG QUIET)
if (NOT slick-queue_FOUND)
    message(STATUS "Fetching slick-queue...")
    include(FetchContent)
    # Disable tests for slick-queue
    set(BUILD_SLICK_QUEUE_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-queue
        GIT_REPOSITORY https://github.com/SlickQuant/slick-queue.git
        GIT_TAG v1.5.0 # See https://github.com/SlickQuant/slick-queue/releases for latest version
    )
    FetchContent_MakeAvailable(slick-queue)
else()
    message(STATUS "Found slick-queue: ${slick-queue_VERSION}")
endif()

find_package(slick-stream-buffer 1.0.5 CONFIG QUIET)
if (NOT slick-stream-buffer_FOUND)
    message(STATUS "Fetching slick-stream-buffer...")
    include(FetchContent)
    set(BUILD_SLICK_STREAM_BUFFER_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-stream-buffer
        GIT_REPOSITORY https://github.com/SlickQuant/slick-stream-buffer.git
        GIT_TAG v1.0.5
    )
    FetchContent_MakeAvailable(slick-stream-buffer)
else()
    message(STATUS "Found slick-stream-buffer: ${slick-stream-buffer_VERSION}")
endif()

find_package(slick-stream-buffer-multiplexer 1.0.1 CONFIG QUIET)
if (NOT slick-stream-buffer-multiplexer_FOUND)
    message(STATUS "Fetching slick-stream-buffer-multiplexer...")
    include(FetchContent)
    set(BUILD_SLICK_STREAM_BUFFER_MULTIPLEXER_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-stream-buffer-multiplexer
        GIT_REPOSITORY https://github.com/SlickQuant/slick-stream-buffer-multiplexer.git
        GIT_TAG v1.0.1
    )
    FetchContent_MakeAvailable(slick-stream-buffer-multiplexer)
else()
    message(STATUS "Found slick-stream-buffer-multiplexer: ${slick-stream-buffer-multiplexer_VERSION}")
endif()

find_package(slick-dynamic-buffer 1.0.1 CONFIG QUIET)
if (NOT slick-dynamic-buffer_FOUND)
    message(STATUS "Fetching slick-dynamic-buffer...")
    include(FetchContent)
    set(BUILD_SLICK_DYNAMIC_BUFFER_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-dynamic-buffer
        GIT_REPOSITORY https://github.com/SlickQuant/slick-dynamic-buffer.git
        GIT_TAG v1.0.1
    )
    FetchContent_MakeAvailable(slick-dynamic-buffer)
else()
    message(STATUS "Found slick-dynamic-buffer: ${slick-dynamic-buffer_VERSION}")
endif()

message(STATUS "OpenSSL: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "Beast: ${boost_beast_CONFIG}")

add_library(slick-net STATIC
    src/http.cpp
    src/http_stream.cpp
    src/websocket.cpp
    src/websocket_stream_buffer.cpp
    src/websocket_stream_buffer_multiplexer.cpp
    src/logging.cpp
)
add_library(slick::net ALIAS slick-net)
# For backward compatiblity
add_library(slick_net ALIAS slick-net)
add_library(slick::slick_net ALIAS slick-net)

target_include_directories(slick-net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(slick-net PUBLIC
    slick::queue
    slick::stream_buffer
    slick::stream_buffer_multiplexer
    slick::dynamic_buffer
    Boost::asio
    Boost::beast
    Boost::context
    OpenSSL::SSL
    OpenSSL::Crypto
)
set_target_properties(slick-net PROPERTIES EXPORT_NAME net)


if (MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")
    add_definitions(-D_WIN32_WINNT=0x0A00)
    set(CMAKE_SUPPRESS_REGENERATION true)   # supress zero_check
    set_target_properties(slick-net PROPERTIES LINK_INCREMENTAL ON)
    target_compile_options(slick-net PRIVATE "/bigobj")
    if (LINK_STATICALLY)
        set_target_properties(slick-net PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
else()
    if(CMAKE_CROSSCOMPILING)
        set(CMAKE_CXX_FLAGS_RELEASE "-O3")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")
    endif()
endif()

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

if (BUILD_SLICK_NET_TESTS)
    message(STATUS "Building slick-net tests")
    enable_testing()
    add_subdirectory(tests)
else()
    message(STATUS "Skipping slick-net tests")
endif()

if(BUILD_SLICK_NET_EXAMPLES)
    message(STATUS "Building slick::net examples")
    add_subdirectory(examples)
else()
    message(STATUS "Skipping slick::net examples")
endif()

# Installation rules
install(DIRECTORY include/ DESTINATION include)

# Install targets and create CMake config files
install(TARGETS slick-net EXPORT slick-net-targets INCLUDES DESTINATION include)

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

message(STATUS "${PROJECT_NAME}: ${PROJECT_VERSION}")
