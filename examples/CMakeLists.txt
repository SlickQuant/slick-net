project(websocket_client_example LANGUAGES CXX)

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

include(FetchContent)
# Disable examples, tests, and benchmarks for slick-logger
set(BUILD_SLICK_LOGGER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SLICK_LOGGER_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SLICK_LOGGER_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    slick-logger
    GIT_REPOSITORY https://github.com/SlickQuant/slick-logger.git
    GIT_TAG v1.0.4
)
FetchContent_MakeAvailable(slick-logger)

add_executable(websocket_client_example websocket_client.cpp)
target_include_directories(websocket_client_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(websocket_client_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)

add_executable(http_client_example http_client.cpp)
target_include_directories(http_client_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(http_client_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)

add_executable(http_stream_client_example http_stream_client.cpp)
target_include_directories(http_stream_client_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(http_stream_client_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)

add_executable(http_awaitable_client_example http_awaitable_client.cpp)
target_include_directories(http_awaitable_client_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(http_awaitable_client_example PRIVATE slick::net slick::logger nlohmann_json::nlohmann_json)