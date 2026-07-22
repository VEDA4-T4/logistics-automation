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
    sorting_control_test
    sorting_control_test.c
    ../Application/Src/sorting_control.c
)

add_executable(
    sorting_motor_tb6612_test
    sorting_motor_tb6612_test.c
    ../Application/Src/sorting_motor_tb6612.c
)

add_executable(
    sorting_gate_mg90s_test
    sorting_gate_mg90s_test.c
    ../Application/Src/sorting_gate_mg90s.c
)

add_executable(
    sorting_control_task_test
    sorting_control_task_test.c
    ../Application/Src/sorting_control.c
    ../Application/Src/sorting_control_task.c
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

add_executable(
    hc_sr04_test
    hc_sr04_test.c
    ../Application/Src/hc_sr04.c
)

add_executable(
    sensor_filter_test
    sensor_filter_test.c
    ../Application/Src/sensor_filter.c
)

add_executable(
    safety_task_test
    safety_task_test.c
    ../Application/Src/safety_task.c
)

add_executable(
    safety_integration_test
    safety_integration_test.c
    ../Application/Src/safety_task.c
    ../Application/Src/input_control.c
    ../Application/Src/input_control_task.c
    ../Application/Src/sorting_control.c
    ../Application/Src/sorting_control_task.c
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
    sorting_control_test
    PRIVATE ../Application/Inc
)

target_include_directories(
    sorting_motor_tb6612_test
    PRIVATE
        fakes
        ../Application/Inc
)

target_include_directories(
    sorting_gate_mg90s_test
    PRIVATE
        fakes
        ../Application/Inc
)

target_include_directories(
    sorting_control_task_test
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

target_include_directories(
    hc_sr04_test
    PRIVATE
        fakes
        ../Application/Inc
)

target_include_directories(
    sensor_filter_test
    PRIVATE ../Application/Inc
)

target_include_directories(
    safety_task_test
    PRIVATE
        fakes
        ../Application/Inc
)

target_include_directories(
    safety_integration_test
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
    sorting_control_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    sorting_motor_tb6612_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    sorting_gate_mg90s_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    sorting_control_task_test
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

set_target_properties(
    hc_sr04_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    sensor_filter_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    safety_task_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    safety_integration_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

target_link_libraries(input_control_test PRIVATE logistics::contracts)
target_link_libraries(sensor_filter_test PRIVATE logistics::contracts)
target_link_libraries(safety_task_test PRIVATE logistics::contracts)
target_link_libraries(safety_integration_test PRIVATE logistics::contracts)
target_link_libraries(input_motor_tb6612_test PRIVATE logistics::contracts)
target_link_libraries(input_control_task_test PRIVATE logistics::contracts)
target_link_libraries(sorting_control_test PRIVATE logistics::contracts)
target_link_libraries(sorting_motor_tb6612_test PRIVATE logistics::contracts)
target_link_libraries(sorting_gate_mg90s_test PRIVATE logistics::contracts)
target_link_libraries(sorting_control_task_test PRIVATE logistics::contracts)
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
        NAME sorting_control_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:sorting_control_test>
    )
    add_test(
        NAME sorting_motor_tb6612_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:sorting_motor_tb6612_test>
    )
    add_test(
        NAME sorting_gate_mg90s_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:sorting_gate_mg90s_test>
    )
    add_test(
        NAME sorting_control_task_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:sorting_control_task_test>
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
    add_test(
        NAME hc_sr04_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:hc_sr04_test>
    )
    add_test(
        NAME sensor_filter_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:sensor_filter_test>
    )
    add_test(
        NAME safety_task_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:safety_task_test>
    )
    add_test(
        NAME safety_integration_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:safety_integration_test>
    )
else()
    add_test(NAME input_control_test COMMAND input_control_test)
    add_test(NAME input_motor_tb6612_test COMMAND input_motor_tb6612_test)
    add_test(NAME input_control_task_test COMMAND input_control_task_test)
    add_test(NAME sorting_control_test COMMAND sorting_control_test)
    add_test(NAME sorting_motor_tb6612_test COMMAND sorting_motor_tb6612_test)
    add_test(NAME sorting_gate_mg90s_test COMMAND sorting_gate_mg90s_test)
    add_test(NAME sorting_control_task_test COMMAND sorting_control_task_test)
    add_test(NAME app_queues_test COMMAND app_queues_test)
    add_test(NAME comm_rx_routing_test COMMAND comm_rx_routing_test)
    add_test(NAME app_comm_tx_test COMMAND app_comm_tx_test)
    add_test(NAME hc_sr04_test COMMAND hc_sr04_test)
    add_test(NAME sensor_filter_test COMMAND sensor_filter_test)
    add_test(NAME safety_task_test COMMAND safety_task_test)
    add_test(NAME safety_integration_test COMMAND safety_integration_test)
endif()
