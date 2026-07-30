add_executable(
    gripper_control_test
    gripper_control_test.c
    ../Application/Src/gripper_control.c
)

target_include_directories(
    gripper_control_test
    PRIVATE
        ../Application/Inc
        ${PROJECT_SOURCE_DIR}/shared/include
)

target_compile_features(gripper_control_test PRIVATE c_std_11)

if(MSVC)
    target_compile_options(gripper_control_test PRIVATE /W4 /WX)
else()
    target_compile_options(gripper_control_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME gripper_control_test COMMAND gripper_control_test)

add_executable(uart_rx_test uart_rx_test.c)
target_include_directories(
    uart_rx_test
    PRIVATE
        fakes
        ../Application/Inc
        ${PROJECT_SOURCE_DIR}/shared/include
)
target_compile_features(uart_rx_test PRIVATE c_std_11)

add_executable(gripper_servo_test gripper_servo_test.c)
target_include_directories(
    gripper_servo_test
    PRIVATE
        fakes
        ../Application/Inc
        ${PROJECT_SOURCE_DIR}/shared/include
)
target_compile_features(gripper_servo_test PRIVATE c_std_11)

add_executable(
    app_comm_tx_test
    app_comm_tx_test.c
    ${PROJECT_SOURCE_DIR}/shared/contracts/uart/uart_codec.c
    ${PROJECT_SOURCE_DIR}/shared/contracts/uart/uart_crc16.c
)
target_include_directories(
    app_comm_tx_test
    PRIVATE
        fakes
        ../Application/Inc
        ${PROJECT_SOURCE_DIR}/shared/include
)
target_compile_features(app_comm_tx_test PRIVATE c_std_11)

add_executable(
    gripper_control_task_test
    gripper_control_task_test.c
    ../Application/Src/gripper_control.c
)
target_include_directories(
    gripper_control_task_test
    PRIVATE
        fakes
        ../Application/Inc
        ${PROJECT_SOURCE_DIR}/shared/include
)
target_compile_features(gripper_control_task_test PRIVATE c_std_11)

foreach(target uart_rx_test gripper_servo_test app_comm_tx_test gripper_control_task_test)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()

add_test(NAME uart_rx_test COMMAND uart_rx_test)
add_test(NAME gripper_servo_test COMMAND gripper_servo_test)
add_test(NAME app_comm_tx_test COMMAND app_comm_tx_test)
add_test(NAME gripper_control_task_test COMMAND gripper_control_task_test)
