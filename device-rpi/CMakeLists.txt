# ---------------------------------------------------------------------------
# Cross-platform libraries and tests.
#
# These build even when the full device-node runtime (mosquitto/CURL/OpenCV) is
# disabled, so the UART transport and the input-node command logic can be unit
# tested on a developer host. The device-node runtime below is skipped unless
# LOGISTICS_BUILD_DEVICE_NODES is enabled.
# ---------------------------------------------------------------------------
include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)

add_library(device_uart_transport common/uart_bridge/uart_transport.cpp)
add_library(logistics::device_uart_transport ALIAS device_uart_transport)
target_include_directories(device_uart_transport PUBLIC common/include)
logistics_enable_warnings(device_uart_transport)

add_library(device_uart_session common/uart_bridge/uart_session.cpp)
add_library(logistics::device_uart_session ALIAS device_uart_session)
target_include_directories(device_uart_session PUBLIC common/include)
target_link_libraries(device_uart_session PUBLIC logistics::device_uart_transport logistics::uart)
logistics_enable_warnings(device_uart_session)

if(LOGISTICS_BUILD_DEVICE_UART_TRANSPORT)
    add_executable(logistics_gripper_uart_roundtrip tools/gripper_uart_roundtrip.cpp)
    target_link_libraries(
        logistics_gripper_uart_roundtrip
        PRIVATE logistics::device_uart_transport logistics::uart
    )
    logistics_enable_warnings(logistics_gripper_uart_roundtrip)
endif()

add_library(device_linetracer_node linetracer-node/linetracer_node.cpp)
add_library(logistics::device_linetracer_node ALIAS device_linetracer_node)
target_include_directories(device_linetracer_node PUBLIC common/include)
target_link_libraries(device_linetracer_node PUBLIC logistics::contracts logistics::device_uart_session)
logistics_enable_warnings(device_linetracer_node)

# Shared by every controller that answers a command with a sequence-matched
# RESPONSE frame instead of an ACK (input conveyor and gripper today).
add_library(device_response_uart_session input-node/input_uart_session.cpp)
add_library(logistics::device_response_uart_session ALIAS device_response_uart_session)
target_include_directories(device_response_uart_session PUBLIC common/include)
target_link_libraries(
    device_response_uart_session
    PUBLIC logistics::contracts logistics::uart logistics::device_uart_transport
)
logistics_enable_warnings(device_response_uart_session)

add_library(device_input_node input-node/input_node.cpp)
add_library(logistics::device_input_node ALIAS device_input_node)
target_include_directories(device_input_node PUBLIC common/include)
target_link_libraries(
    device_input_node
    PUBLIC logistics::contracts logistics::uart logistics::device_response_uart_session
)
logistics_enable_warnings(device_input_node)

add_library(
    device_gripper_node
    gripper-node/gripper_kinematics.cpp
    gripper-node/gripper_pose_config.cpp
    gripper-node/gripper_node.cpp
)
add_library(logistics::device_gripper_node ALIAS device_gripper_node)
target_include_directories(device_gripper_node PUBLIC common/include)
target_link_libraries(
    device_gripper_node
    PUBLIC logistics::contracts logistics::uart logistics::device_response_uart_session
)
logistics_enable_warnings(device_gripper_node)

add_library(device_sorting_node sorting-node/sorting_node.cpp)
add_library(logistics::device_sorting_node ALIAS device_sorting_node)
target_include_directories(device_sorting_node PUBLIC common/include)
target_link_libraries(device_sorting_node PUBLIC logistics::contracts logistics::device_uart_session)
logistics_enable_warnings(device_sorting_node)

add_library(device_control_state common/device_control/device_control_state.cpp)
add_library(logistics::device_control_state ALIAS device_control_state)
target_include_directories(device_control_state PUBLIC common/include)
target_link_libraries(device_control_state PUBLIC logistics::contracts)
logistics_enable_warnings(device_control_state)

if(BUILD_TESTING)
    add_executable(device_uart_transport_test tests/uart_transport_test.cpp)
    target_link_libraries(device_uart_transport_test PRIVATE logistics::device_uart_transport)
    logistics_enable_warnings(device_uart_transport_test)
    add_test(NAME device_uart_transport_test COMMAND device_uart_transport_test)

    add_executable(device_uart_session_test tests/uart_session_test.cpp)
    target_link_libraries(device_uart_session_test PRIVATE logistics::device_uart_session)
    logistics_enable_warnings(device_uart_session_test)
    add_test(NAME device_uart_session_test COMMAND device_uart_session_test)
    add_executable(device_linetracer_node_test tests/linetracer_node_test.cpp)
    target_link_libraries(device_linetracer_node_test PRIVATE logistics::device_linetracer_node)
    logistics_enable_warnings(device_linetracer_node_test)
    add_test(NAME device_linetracer_node_test COMMAND device_linetracer_node_test)

    add_executable(device_input_uart_session_test tests/input_uart_session_test.cpp)
    target_link_libraries(device_input_uart_session_test PRIVATE logistics::device_input_node)
    logistics_enable_warnings(device_input_uart_session_test)
    add_test(NAME device_input_uart_session_test COMMAND device_input_uart_session_test)

    add_executable(device_input_node_test tests/input_node_test.cpp)
    target_link_libraries(device_input_node_test PRIVATE logistics::device_input_node)
    logistics_enable_warnings(device_input_node_test)
    add_test(NAME device_input_node_test COMMAND device_input_node_test)

    add_executable(device_gripper_node_test tests/gripper_node_test.cpp)
    target_link_libraries(device_gripper_node_test PRIVATE logistics::device_gripper_node)
    logistics_enable_warnings(device_gripper_node_test)
    add_test(NAME device_gripper_node_test COMMAND device_gripper_node_test)

    add_executable(device_gripper_kinematics_test tests/gripper_kinematics_test.cpp)
    target_link_libraries(device_gripper_kinematics_test PRIVATE logistics::device_gripper_node)
    logistics_enable_warnings(device_gripper_kinematics_test)
    add_test(NAME device_gripper_kinematics_test COMMAND device_gripper_kinematics_test)

    add_executable(device_sorting_node_test tests/sorting_node_test.cpp)
    target_link_libraries(device_sorting_node_test PRIVATE logistics::device_sorting_node)
    logistics_enable_warnings(device_sorting_node_test)
    add_test(NAME device_sorting_node_test COMMAND device_sorting_node_test)

    add_executable(device_node_command_queue_test tests/node_command_queue_test.cpp)
    target_include_directories(device_node_command_queue_test PRIVATE common/include)
    target_link_libraries(device_node_command_queue_test PRIVATE logistics::contracts)
    logistics_enable_warnings(device_node_command_queue_test)
    add_test(NAME device_node_command_queue_test COMMAND device_node_command_queue_test)

    add_executable(device_control_policy_test tests/device_control_policy_test.cpp)
    target_include_directories(device_control_policy_test PRIVATE common/include)
    target_link_libraries(device_control_policy_test PRIVATE logistics::contracts logistics::device_control_state)
    logistics_enable_warnings(device_control_policy_test)
    add_test(NAME device_control_policy_test COMMAND device_control_policy_test)
endif()

if(NOT LOGISTICS_BUILD_DEVICE_NODES)
    return()
endif()

add_library(
    device_mqtt_core
    common/config/mqtt_node_config.cpp
    common/mqtt_client/mqtt_message_processor.cpp
    common/mqtt_client/mqtt_publish_spool.cpp
    common/mqtt_client/mqtt_time.cpp
)
add_library(logistics::device_mqtt_core ALIAS device_mqtt_core)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(device_log_spool common/src/log_spool_uploader.cpp)
add_library(logistics::device_log_spool ALIAS device_log_spool)
target_include_directories(device_log_spool PUBLIC common/include)
target_link_libraries(
    device_log_spool
    PUBLIC logistics::contracts
    PRIVATE CURL::libcurl OpenSSL::Crypto Threads::Threads
)

target_include_directories(device_mqtt_core PUBLIC common/include)
target_link_libraries(device_mqtt_core PUBLIC logistics::contracts)

logistics_enable_warnings(device_mqtt_core)
logistics_enable_warnings(device_log_spool)

add_library(
    device_image_upload
    common/src/image_uploader.cpp
    common/src/curl_image_upload_transport.cpp
)
add_library(logistics::device_image_upload ALIAS device_image_upload)
target_include_directories(device_image_upload PUBLIC common/include)
target_link_libraries(
    device_image_upload
    PUBLIC logistics::contracts
    PRIVATE CURL::libcurl OpenSSL::Crypto Threads::Threads
)
logistics_enable_warnings(device_image_upload)

add_library(device_status common/device_status/device_status.cpp)
add_library(logistics::device_status ALIAS device_status)
target_include_directories(device_status PUBLIC common/include)
target_link_libraries(device_status PUBLIC logistics::contracts)
logistics_enable_warnings(device_status)

add_library(vision_mqtt_workflow vision-node/vision_mqtt_workflow.cpp)
add_library(logistics::vision_mqtt_workflow ALIAS vision_mqtt_workflow)
target_include_directories(vision_mqtt_workflow PUBLIC vision-node)
target_link_libraries(vision_mqtt_workflow PUBLIC logistics::contracts)
logistics_enable_warnings(vision_mqtt_workflow)

if(LOGISTICS_BUILD_VISION_NODE)
    find_package(OpenCV 4.10.0 EXACT REQUIRED COMPONENTS core dnn highgui imgcodecs imgproc objdetect videoio)

    add_library(
        vision_processing_config
        vision-node/vision_processing_config.cpp
        vision-node/vision_processing_config.hpp
    )
    add_library(logistics::vision_processing_config ALIAS vision_processing_config)
    target_include_directories(vision_processing_config PUBLIC vision-node)
    logistics_enable_warnings(vision_processing_config)

    add_library(
        vision_failure_frame_store
        vision-node/failure_frame_store.cpp
        vision-node/failure_frame_store.hpp
    )
    add_library(logistics::vision_failure_frame_store ALIAS vision_failure_frame_store)
    target_include_directories(vision_failure_frame_store PUBLIC vision-node ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(vision_failure_frame_store PUBLIC logistics::vision_processing_config ${OpenCV_LIBS})
    logistics_enable_warnings(vision_failure_frame_store)

    add_library(
        vision_detection
        vision-node/detection.cpp
        vision-node/detection.hpp
    )
    add_library(logistics::vision_detection ALIAS vision_detection)
    target_include_directories(vision_detection PUBLIC vision-node ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(vision_detection PUBLIC logistics::vision_processing_config ${OpenCV_LIBS})
    logistics_enable_warnings(vision_detection)
endif()

add_library(
    device_runtime
    common/src/node_runtime.cpp
)
add_library(logistics::device_runtime ALIAS device_runtime)

target_include_directories(device_runtime PUBLIC common/include)
target_link_libraries(
    device_runtime
    PUBLIC logistics::contracts
    PRIVATE
        logistics::device_mqtt_core
        logistics::device_status
        logistics::device_log_spool
)

logistics_enable_warnings(device_runtime)

if(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT)
    if(TARGET PkgConfig::MOSQUITTO)
        set(LOGISTICS_DEVICE_MOSQUITTO_TARGET PkgConfig::MOSQUITTO)
    elseif(TARGET Mosquitto::Mosquitto)
        set(LOGISTICS_DEVICE_MOSQUITTO_TARGET Mosquitto::Mosquitto)
    endif()

    if(NOT LOGISTICS_DEVICE_MOSQUITTO_TARGET)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(DEVICE_MOSQUITTO QUIET IMPORTED_TARGET libmosquitto)
        endif()
        if(TARGET PkgConfig::DEVICE_MOSQUITTO)
            set(LOGISTICS_DEVICE_MOSQUITTO_TARGET PkgConfig::DEVICE_MOSQUITTO)
        else()
            find_path(DEVICE_MOSQUITTO_INCLUDE_DIR mosquitto.h)
            find_library(DEVICE_MOSQUITTO_LIBRARY NAMES mosquitto)
            if(DEVICE_MOSQUITTO_INCLUDE_DIR AND DEVICE_MOSQUITTO_LIBRARY)
                add_library(DeviceMosquitto::Mosquitto UNKNOWN IMPORTED)
                set_target_properties(
                    DeviceMosquitto::Mosquitto
                    PROPERTIES
                        IMPORTED_LOCATION "${DEVICE_MOSQUITTO_LIBRARY}"
                        INTERFACE_INCLUDE_DIRECTORIES "${DEVICE_MOSQUITTO_INCLUDE_DIR}"
                )
                set(LOGISTICS_DEVICE_MOSQUITTO_TARGET DeviceMosquitto::Mosquitto)
            endif()
        endif()
    endif()

    if(NOT LOGISTICS_DEVICE_MOSQUITTO_TARGET)
        message(
            FATAL_ERROR
                "libmosquitto was not found for device nodes. Install libmosquitto-dev, "
                "or configure with -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=OFF."
        )
    endif()

    add_library(device_mqtt_mosquitto common/mqtt_client/mqtt_node_client.cpp)
    add_library(logistics::device_mqtt_mosquitto ALIAS device_mqtt_mosquitto)
    target_include_directories(device_mqtt_mosquitto PUBLIC common/include)
    target_link_libraries(
        device_mqtt_mosquitto
        PUBLIC
            logistics::device_mqtt_core
            logistics::device_status
        PRIVATE ${LOGISTICS_DEVICE_MOSQUITTO_TARGET}
    )
    logistics_enable_warnings(device_mqtt_mosquitto)

    target_compile_definitions(device_runtime PRIVATE LOGISTICS_DEVICE_MQTT_ENABLED=1)
    target_link_libraries(device_runtime PRIVATE logistics::device_mqtt_mosquitto)
endif()

function(add_device_node target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE logistics::device_runtime)
    logistics_enable_warnings(${target_name})
endfunction()

if(LOGISTICS_BUILD_INPUT_NODE)
    add_device_node(logistics_input_node input-node/main.cpp)

    if(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT)
        target_compile_definitions(logistics_input_node PRIVATE LOGISTICS_INPUT_DAEMON_ENABLED=1)
        target_link_libraries(
            logistics_input_node
            PRIVATE
                logistics::device_input_node
                logistics::device_mqtt_mosquitto
                logistics::device_status
        )
    endif()
endif()

if(LOGISTICS_BUILD_SORTING_NODE)
    add_device_node(logistics_sorting_node sorting-node/main.cpp)
    target_link_libraries(logistics_sorting_node PRIVATE logistics::device_sorting_node logistics::device_uart_session)
endif()

if(LOGISTICS_BUILD_GRIPPER_NODE)
    add_device_node(logistics_gripper_node gripper-node/main.cpp)

    if(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT)
        target_compile_definitions(logistics_gripper_node PRIVATE LOGISTICS_GRIPPER_DAEMON_ENABLED=1)
        target_link_libraries(
            logistics_gripper_node
            PRIVATE
                logistics::device_gripper_node
                logistics::device_mqtt_mosquitto
                logistics::device_status
        )
    endif()
endif()

if(LOGISTICS_BUILD_LINETRACER_NODE)
    add_device_node(logistics_linetracer_node linetracer-node/main.cpp)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(vedauart_cli_test common/uart_bridge/vedauart_cli_test.c)
    target_link_libraries(vedauart_cli_test PRIVATE logistics::uart)
    logistics_enable_warnings(vedauart_cli_test)
endif()

if(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT AND TARGET logistics_linetracer_node)
    target_compile_definitions(logistics_linetracer_node PRIVATE LOGISTICS_LINETRACER_DAEMON_ENABLED=1)
    target_link_libraries(
        logistics_linetracer_node
        PRIVATE
            logistics::device_linetracer_node
            logistics::device_mqtt_mosquitto
            logistics::device_status
    )
endif()

if(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT AND TARGET logistics_sorting_node)
    target_compile_definitions(logistics_sorting_node PRIVATE LOGISTICS_SORTING_DAEMON_ENABLED=1)
    target_link_libraries(
        logistics_sorting_node
        PRIVATE logistics::device_mqtt_mosquitto logistics::device_status
    )
endif()

if(BUILD_TESTING)
    add_executable(device_mqtt_message_processor_test tests/mqtt_message_processor_test.cpp)
    target_link_libraries(device_mqtt_message_processor_test PRIVATE logistics::device_mqtt_core)
    logistics_enable_warnings(device_mqtt_message_processor_test)
    add_test(NAME device_mqtt_message_processor_test COMMAND device_mqtt_message_processor_test)

    add_executable(device_mqtt_node_config_test tests/mqtt_node_config_test.cpp)
    target_link_libraries(device_mqtt_node_config_test PRIVATE logistics::device_mqtt_core)
    logistics_enable_warnings(device_mqtt_node_config_test)
    add_test(NAME device_mqtt_node_config_test COMMAND device_mqtt_node_config_test)

    add_executable(device_mqtt_publish_spool_test tests/mqtt_publish_spool_test.cpp)
    target_link_libraries(device_mqtt_publish_spool_test PRIVATE logistics::device_mqtt_core)
    logistics_enable_warnings(device_mqtt_publish_spool_test)
    add_test(NAME device_mqtt_publish_spool_test COMMAND device_mqtt_publish_spool_test)

    add_executable(device_status_test tests/device_status_test.cpp)
    target_link_libraries(device_status_test PRIVATE logistics::device_status)
    logistics_enable_warnings(device_status_test)
    add_test(NAME device_status_test COMMAND device_status_test)

    add_executable(device_log_spool_test tests/log_spool_uploader_test.cpp)
    target_link_libraries(device_log_spool_test PRIVATE logistics::device_log_spool)
    logistics_enable_warnings(device_log_spool_test)
    add_test(NAME device_log_spool_test COMMAND device_log_spool_test)

    add_executable(vision_mqtt_workflow_test tests/vision_mqtt_workflow_test.cpp)
    target_link_libraries(
        vision_mqtt_workflow_test
        PRIVATE logistics::device_control_state logistics::vision_mqtt_workflow
    )
    logistics_enable_warnings(vision_mqtt_workflow_test)
    add_test(NAME vision_mqtt_workflow_test COMMAND vision_mqtt_workflow_test)

    if(LOGISTICS_BUILD_VISION_NODE)
        add_executable(vision_processing_config_test tests/vision_processing_config_test.cpp)
        target_link_libraries(vision_processing_config_test PRIVATE logistics::vision_processing_config)
        logistics_enable_warnings(vision_processing_config_test)
        add_test(NAME vision_processing_config_test COMMAND vision_processing_config_test)

        add_executable(vision_detection_test tests/vision_detection_test.cpp)
        target_link_libraries(vision_detection_test PRIVATE logistics::vision_detection)
        logistics_enable_warnings(vision_detection_test)
        add_test(NAME vision_detection_test COMMAND vision_detection_test)

        add_executable(vision_super_resolution_preview_test tests/vision_super_resolution_preview_test.cpp)
        target_link_libraries(vision_super_resolution_preview_test PRIVATE logistics::vision_detection)
        logistics_enable_warnings(vision_super_resolution_preview_test)
        add_test(NAME vision_super_resolution_preview_test COMMAND vision_super_resolution_preview_test)

        add_executable(vision_failure_frame_store_test tests/vision_failure_frame_store_test.cpp)
        target_link_libraries(vision_failure_frame_store_test PRIVATE logistics::vision_failure_frame_store)
        logistics_enable_warnings(vision_failure_frame_store_test)
        add_test(NAME vision_failure_frame_store_test COMMAND vision_failure_frame_store_test)

    endif()

    add_executable(device_image_uploader_test tests/image_uploader_test.cpp)
    target_link_libraries(device_image_uploader_test PRIVATE logistics::device_image_upload)
    logistics_enable_warnings(device_image_uploader_test)
    add_test(NAME device_image_uploader_test COMMAND device_image_uploader_test)

    add_executable(vision_image_upload_task_test tests/image_upload_task_test.cpp)
    target_include_directories(vision_image_upload_task_test PRIVATE vision-node)
    target_link_libraries(vision_image_upload_task_test PRIVATE Threads::Threads)
    logistics_enable_warnings(vision_image_upload_task_test)
    add_test(NAME vision_image_upload_task_test COMMAND vision_image_upload_task_test)
endif()
if(LOGISTICS_BUILD_VISION_NODE)
    add_device_node(logistics_vision_node vision-node/main.cpp)
    target_link_libraries(
        logistics_vision_node
        PRIVATE
            logistics::device_control_state
            logistics::vision_detection
            logistics::vision_failure_frame_store
            logistics::vision_mqtt_workflow
    )

    add_executable(logistics_vision_benchmark vision-node/vision_benchmark.cpp)
    target_link_libraries(logistics_vision_benchmark PRIVATE logistics::vision_detection)
    logistics_enable_warnings(logistics_vision_benchmark)
    if(BUILD_TESTING)
        add_test(NAME vision_benchmark_help COMMAND logistics_vision_benchmark --help)
        add_test(
            NAME vision_benchmark_visual_output
            COMMAND
                ${CMAKE_COMMAND}
                -DBENCHMARK_EXECUTABLE=$<TARGET_FILE:logistics_vision_benchmark>
                -DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/vision-benchmark-visual-test
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/vision_benchmark_visual_test.cmake
        )
    endif()

    if(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT)
        target_compile_definitions(logistics_vision_node PRIVATE LOGISTICS_VISION_MQTT_ENABLED=1)
        target_link_libraries(
            logistics_vision_node
            PRIVATE logistics::device_mqtt_mosquitto logistics::device_status logistics::device_image_upload
        )
    endif()
endif()
