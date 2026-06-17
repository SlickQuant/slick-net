project(websocket_client_example LANGUAGES CXX)

find_package(OpenSSL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(slick-logger 1.0.9 CONFIG QUIET)

if (NOT slick-logger_FOUND)
    message(STATUS "Fetching slick-logger...")
    include(FetchContent)
    # Disable examples, tests, and benchmarks for slick-logger
    set(BUILD_SLICK_LOGGER_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_LOGGER_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SLICK_LOGGER_BENCHMARKS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        slick-logger
        GIT_REPOSITORY https://github.com/SlickQuant/slick-logger.git
        GIT_TAG v1.0.9
    )
    FetchContent_MakeAvailable(slick-logger)
else()
    message(STATUS "Found slick-logger: ${slick-logger_VERSION}")
endif()

add_executable(websocket_client_example websocket_client.cpp)
target_include_directories(websocket_client_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(websocket_client_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)

add_executable(websocket_with_stream_buffer_example websocket_with_stream_buffer.cpp)
target_include_directories(websocket_with_stream_buffer_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(websocket_with_stream_buffer_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)

add_executable(websocket_with_stream_buffer_multiplexer_example websocket_with_stream_buffer_multiplexer.cpp)
target_include_directories(websocket_with_stream_buffer_multiplexer_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(websocket_with_stream_buffer_multiplexer_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)

add_executable(websocket_with_custom_buffer_example websocket_with_custom_buffer.cpp)
target_include_directories(websocket_with_custom_buffer_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(websocket_with_custom_buffer_example PRIVATE slick::net slick::logger)
if (MSVC)
    target_compile_options(websocket_with_custom_buffer_example PRIVATE "/bigobj")
endif()

add_executable(http_client_example http_client.cpp)
target_include_directories(http_client_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(http_client_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)

add_executable(http_stream_client_example http_stream_client.cpp)
target_include_directories(http_stream_client_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(http_stream_client_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)

add_executable(http_awaitable_client_example http_awaitable_client.cpp)
target_include_directories(http_awaitable_client_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(http_awaitable_client_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)
