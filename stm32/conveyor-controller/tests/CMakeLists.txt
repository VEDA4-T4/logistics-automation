add_executable(
    input_control_test
    input_control_test.c
    ../Application/Src/input_control.c
)

add_executable(
    input_motor_tb6612_test
    input_motor_tb6612_test.c
    ../Application/Src/input_motor_tb6612.c
)

add_executable(
    input_control_task_test
    input_control_task_test.c
    ../Application/Src/input_control.c
    ../Application/Src/input_control_task.c
)

add_executable(
    app_queues_test
    app_queues_test.c
    ../Application/Src/app_queues.c
)

add_executable(
    comm_rx_routing_test
    comm_rx_routing_test.c
    ../Application/Src/comm_rx_task.c
)

add_executable(
    app_comm_tx_test
    app_comm_tx_test.c
    ../Application/Src/app_comm_tx.c
)

target_include_directories(
    input_control_test
    PRIVATE ../Application/Inc
)

target_include_directories(
    input_motor_tb6612_test
    PRIVATE
        fakes
        ../Application/Inc
)

target_include_directories(
    input_control_task_test
    PRIVATE
        fakes
        ../Application/Inc
)

target_include_directories(
    app_queues_test
    PRIVATE
        fakes
        ../Application/Inc
)

target_include_directories(
    comm_rx_routing_test
    PRIVATE
        fakes
        ../Application/Inc
)

target_include_directories(
    app_comm_tx_test
    PRIVATE
        fakes
        ../Application/Inc
)

set_target_properties(
    input_control_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    input_motor_tb6612_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    input_control_task_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    app_queues_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    comm_rx_routing_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    app_comm_tx_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

target_link_libraries(input_control_test PRIVATE logistics::contracts)
target_link_libraries(input_motor_tb6612_test PRIVATE logistics::contracts)
target_link_libraries(input_control_task_test PRIVATE logistics::contracts)
target_link_libraries(app_queues_test PRIVATE logistics::contracts)
target_link_libraries(comm_rx_routing_test PRIVATE logistics::uart)
target_link_libraries(app_comm_tx_test PRIVATE logistics::uart)

if(WIN32)
    get_filename_component(INPUT_CONTROL_COMPILER_RUNTIME_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    add_test(
        NAME input_control_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:input_control_test>
    )
    add_test(
        NAME input_motor_tb6612_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:input_motor_tb6612_test>
    )
    add_test(
        NAME input_control_task_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:input_control_task_test>
    )
    add_test(
        NAME app_queues_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:app_queues_test>
    )
    add_test(
        NAME comm_rx_routing_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:comm_rx_routing_test>
    )
    add_test(
        NAME app_comm_tx_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:app_comm_tx_test>
    )
else()
    add_test(NAME input_control_test COMMAND input_control_test)
    add_test(NAME input_motor_tb6612_test COMMAND input_motor_tb6612_test)
    add_test(NAME input_control_task_test COMMAND input_control_task_test)
    add_test(NAME app_queues_test COMMAND app_queues_test)
    add_test(NAME comm_rx_routing_test COMMAND comm_rx_routing_test)
    add_test(NAME app_comm_tx_test COMMAND app_comm_tx_test)
endif()
