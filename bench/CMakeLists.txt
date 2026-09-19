add_executable(write_chain_bench write_chain_bench.cpp)
target_link_libraries(write_chain_bench PRIVATE slick::net)

set_target_properties(write_chain_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bench
)
