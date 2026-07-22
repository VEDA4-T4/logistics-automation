add_executable(
    linetracer_comm_rx_logic_test
    comm_rx_logic_test.cpp
    ../../Application/Src/comm_rx_dispatch.c
    ../../Application/Src/comm_rx_logic.c
)

target_include_directories(
    linetracer_comm_rx_logic_test
    PRIVATE ../../Application/Inc
)

target_link_libraries(linetracer_comm_rx_logic_test PRIVATE logistics::uart)

if(WIN32)
    get_filename_component(LOGISTICS_COMM_RX_TEST_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_test(
        NAME linetracer_comm_rx_logic_test
        COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_COMM_RX_TEST_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:linetracer_comm_rx_logic_test>
    )
else()
    add_test(NAME linetracer_comm_rx_logic_test COMMAND linetracer_comm_rx_logic_test)
endif()
