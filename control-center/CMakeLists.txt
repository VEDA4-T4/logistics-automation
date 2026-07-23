cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(logistics_control_center_project LANGUAGES C CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    include(CTest)

    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../shared"
        "${CMAKE_CURRENT_BINARY_DIR}/shared"
    )
endif()

find_package(Qt6 6.10 REQUIRED COMPONENTS Mqtt Multimedia MultimediaWidgets Network Widgets)

qt_standard_project_setup()

qt_add_executable(
    logistics_control_center
    src/main.cpp
    src/main_window.cpp
    src/command_response.cpp
    src/current_product_state.cpp
    src/mqtt_client.cpp
    src/mqtt_message_validator.cpp
    src/operational_log_panel.cpp
    src/operational_log_state.cpp
    src/operations_dashboard_panel.cpp
    src/operations_dashboard_state.cpp
    src/product_result_panel.cpp
    src/process_control_panel.cpp
    src/ui_dialog.cpp
    include/logistics/control_center/main_window.hpp
    include/logistics/control_center/command_response.hpp
    include/logistics/control_center/current_product_state.hpp
    include/logistics/control_center/mqtt_client.hpp
    include/logistics/control_center/mqtt_message_validator.hpp
    include/logistics/control_center/operational_log_panel.hpp
    include/logistics/control_center/operational_log_state.hpp
    include/logistics/control_center/operations_dashboard_panel.hpp
    include/logistics/control_center/operations_dashboard_state.hpp
    include/logistics/control_center/product_result_panel.hpp
    include/logistics/control_center/process_control_panel.hpp
    include/logistics/control_center/process_control_state.hpp
    include/logistics/control_center/ui_dialog.hpp
)

add_custom_target(
    logistics_control_center_config ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/config"
    COMMAND
        ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/config"
        "${CMAKE_CURRENT_BINARY_DIR}/config"
    COMMENT "Copying control-center configuration"
    VERBATIM
)
add_dependencies(logistics_control_center logistics_control_center_config)

target_include_directories(logistics_control_center PRIVATE include)
target_link_libraries(
    logistics_control_center
    PRIVATE
        Qt6::Mqtt
        Qt6::Multimedia
        Qt6::MultimediaWidgets
        Qt6::Network
        Qt6::Widgets
        logistics::contracts
)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectWarnings.cmake")
logistics_enable_warnings(logistics_control_center)

if(BUILD_TESTING)
    add_executable(
        control_center_mqtt_validator_test
        tests/mqtt_message_validator_test.cpp
        src/mqtt_message_validator.cpp
        include/logistics/control_center/mqtt_message_validator.hpp
    )
    target_include_directories(control_center_mqtt_validator_test PRIVATE include)
    target_link_libraries(control_center_mqtt_validator_test PRIVATE Qt6::Core logistics::contracts)
    logistics_enable_warnings(control_center_mqtt_validator_test)
    add_test(NAME control_center_mqtt_validator_test COMMAND control_center_mqtt_validator_test)

    add_executable(
        control_center_command_response_test
        tests/command_response_test.cpp
        src/command_response.cpp
        include/logistics/control_center/command_response.hpp
    )
    target_include_directories(control_center_command_response_test PRIVATE include)
    target_link_libraries(control_center_command_response_test PRIVATE Qt6::Core logistics::contracts)
    logistics_enable_warnings(control_center_command_response_test)
    add_test(NAME control_center_command_response_test COMMAND control_center_command_response_test)

    add_executable(
        control_center_current_product_state_test
        tests/current_product_state_test.cpp
        src/current_product_state.cpp
        include/logistics/control_center/current_product_state.hpp
    )
    target_include_directories(control_center_current_product_state_test PRIVATE include)
    target_link_libraries(control_center_current_product_state_test PRIVATE Qt6::Core logistics::contracts)
    logistics_enable_warnings(control_center_current_product_state_test)
    add_test(NAME control_center_current_product_state_test COMMAND control_center_current_product_state_test)

    add_executable(
        control_center_process_control_state_test
        tests/process_control_state_test.cpp
        include/logistics/control_center/process_control_state.hpp
    )
    target_include_directories(control_center_process_control_state_test PRIVATE include)
    logistics_enable_warnings(control_center_process_control_state_test)
    add_test(NAME control_center_process_control_state_test COMMAND control_center_process_control_state_test)

    add_executable(
        control_center_operational_log_state_test
        tests/operational_log_state_test.cpp
        src/operational_log_state.cpp
        include/logistics/control_center/operational_log_state.hpp
    )
    target_include_directories(control_center_operational_log_state_test PRIVATE include)
    target_link_libraries(control_center_operational_log_state_test PRIVATE Qt6::Core logistics::contracts)
    logistics_enable_warnings(control_center_operational_log_state_test)
    add_test(NAME control_center_operational_log_state_test COMMAND control_center_operational_log_state_test)

    add_executable(
        control_center_operational_log_panel_test
        tests/operational_log_panel_test.cpp
        src/operational_log_panel.cpp
        src/operational_log_state.cpp
        include/logistics/control_center/operational_log_panel.hpp
        include/logistics/control_center/operational_log_state.hpp
    )
    target_include_directories(control_center_operational_log_panel_test PRIVATE include)
    target_link_libraries(control_center_operational_log_panel_test PRIVATE Qt6::Widgets logistics::contracts)
    logistics_enable_warnings(control_center_operational_log_panel_test)
    add_test(NAME control_center_operational_log_panel_test COMMAND control_center_operational_log_panel_test)

    add_executable(
        control_center_operations_dashboard_state_test
        tests/operations_dashboard_state_test.cpp
        src/operations_dashboard_state.cpp
        include/logistics/control_center/operations_dashboard_state.hpp
    )
    target_include_directories(control_center_operations_dashboard_state_test PRIVATE include)
    target_link_libraries(control_center_operations_dashboard_state_test PRIVATE Qt6::Core logistics::contracts)
    logistics_enable_warnings(control_center_operations_dashboard_state_test)
    add_test(
        NAME control_center_operations_dashboard_state_test
        COMMAND control_center_operations_dashboard_state_test
    )

    add_executable(
        control_center_operations_dashboard_panel_test
        tests/operations_dashboard_panel_test.cpp
        src/operations_dashboard_panel.cpp
        src/operations_dashboard_state.cpp
        include/logistics/control_center/operations_dashboard_panel.hpp
        include/logistics/control_center/operations_dashboard_state.hpp
    )
    target_include_directories(control_center_operations_dashboard_panel_test PRIVATE include)
    target_link_libraries(
        control_center_operations_dashboard_panel_test
        PRIVATE Qt6::Widgets logistics::contracts
    )
    logistics_enable_warnings(control_center_operations_dashboard_panel_test)
    add_test(
        NAME control_center_operations_dashboard_panel_test
        COMMAND control_center_operations_dashboard_panel_test
    )

    add_executable(
        control_center_ui_dialog_test
        tests/ui_dialog_test.cpp
        src/ui_dialog.cpp
        include/logistics/control_center/ui_dialog.hpp
    )
    target_include_directories(control_center_ui_dialog_test PRIVATE include)
    target_link_libraries(control_center_ui_dialog_test PRIVATE Qt6::Widgets)
    logistics_enable_warnings(control_center_ui_dialog_test)
    add_test(NAME control_center_ui_dialog_test COMMAND control_center_ui_dialog_test)

    add_executable(
        control_center_mqtt_reconnect_test
        tests/mqtt_client_reconnect_test.cpp
        src/mqtt_client.cpp
        src/mqtt_message_validator.cpp
        include/logistics/control_center/mqtt_client.hpp
        include/logistics/control_center/mqtt_message_validator.hpp
    )
    target_include_directories(control_center_mqtt_reconnect_test PRIVATE include)
    target_link_libraries(
        control_center_mqtt_reconnect_test
        PRIVATE Qt6::Core Qt6::Mqtt logistics::contracts
    )
    logistics_enable_warnings(control_center_mqtt_reconnect_test)
    add_test(NAME control_center_mqtt_reconnect_test COMMAND control_center_mqtt_reconnect_test)

    add_executable(
        control_center_mqtt_integration_test
        tests/mqtt_client_integration_test.cpp
        src/mqtt_client.cpp
        src/mqtt_message_validator.cpp
        include/logistics/control_center/mqtt_client.hpp
        include/logistics/control_center/mqtt_message_validator.hpp
    )
    target_include_directories(control_center_mqtt_integration_test PRIVATE include)
    target_link_libraries(
        control_center_mqtt_integration_test
        PRIVATE Qt6::Core Qt6::Mqtt logistics::contracts
    )
    logistics_enable_warnings(control_center_mqtt_integration_test)
endif()
