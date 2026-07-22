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

if(BUILD_TESTING)
    add_executable(device_uart_transport_test tests/uart_transport_test.cpp)
    target_link_libraries(device_uart_transport_test PRIVATE logistics::device_uart_transport)
    logistics_enable_warnings(device_uart_transport_test)
    add_test(NAME device_uart_transport_test COMMAND device_uart_transport_test)

    add_executable(device_uart_session_test tests/uart_session_test.cpp)
    target_link_libraries(device_uart_session_test PRIVATE logistics::device_uart_session)
    logistics_enable_warnings(device_uart_session_test)
    add_test(NAME device_uart_session_test COMMAND device_uart_session_test)
endif()

if(NOT LOGISTICS_BUILD_DEVICE_NODES)
    return()
endif()

add_library(
    device_mqtt_core
    common/config/mqtt_node_config.cpp
    common/mqtt_client/mqtt_message_processor.cpp
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

add_device_node(logistics_input_node input-node/main.cpp)
add_device_node(logistics_vision_node vision-node/main.cpp)
add_device_node(logistics_sorting_node sorting-node/main.cpp)
add_device_node(logistics_linetracer_node linetracer-node/main.cpp)

if(BUILD_TESTING)
    add_executable(device_mqtt_message_processor_test tests/mqtt_message_processor_test.cpp)
    target_link_libraries(device_mqtt_message_processor_test PRIVATE logistics::device_mqtt_core)
    logistics_enable_warnings(device_mqtt_message_processor_test)
    add_test(NAME device_mqtt_message_processor_test COMMAND device_mqtt_message_processor_test)

    add_executable(device_mqtt_node_config_test tests/mqtt_node_config_test.cpp)
    target_link_libraries(device_mqtt_node_config_test PRIVATE logistics::device_mqtt_core)
    logistics_enable_warnings(device_mqtt_node_config_test)
    add_test(NAME device_mqtt_node_config_test COMMAND device_mqtt_node_config_test)

    add_executable(device_status_test tests/device_status_test.cpp)
    target_link_libraries(device_status_test PRIVATE logistics::device_status)
    logistics_enable_warnings(device_status_test)
    add_test(NAME device_status_test COMMAND device_status_test)

    add_executable(device_log_spool_test tests/log_spool_uploader_test.cpp)
    target_link_libraries(device_log_spool_test PRIVATE logistics::device_log_spool)
    logistics_enable_warnings(device_log_spool_test)
    add_test(NAME device_log_spool_test COMMAND device_log_spool_test)

    add_executable(vision_mqtt_workflow_test tests/vision_mqtt_workflow_test.cpp)
    target_link_libraries(vision_mqtt_workflow_test PRIVATE logistics::vision_mqtt_workflow)
    logistics_enable_warnings(vision_mqtt_workflow_test)
    add_test(NAME vision_mqtt_workflow_test COMMAND vision_mqtt_workflow_test)

    add_executable(device_image_uploader_test tests/image_uploader_test.cpp)
    target_link_libraries(device_image_uploader_test PRIVATE logistics::device_image_upload)
    logistics_enable_warnings(device_image_uploader_test)
    add_test(NAME device_image_uploader_test COMMAND device_image_uploader_test)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(vedauart_cli_test common/uart_bridge/vedauart_cli_test.c)
    target_link_libraries(vedauart_cli_test PRIVATE logistics::uart)
    logistics_enable_warnings(vedauart_cli_test)
endif()

target_sources(
    logistics_vision_node
    PRIVATE
        vision-node/detection.cpp
        vision-node/detection.hpp
)

find_package(OpenCV 4.10.0 EXACT REQUIRED COMPONENTS core highgui imgcodecs imgproc objdetect videoio)
target_include_directories(logistics_vision_node PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(logistics_vision_node PRIVATE ${OpenCV_LIBS})
target_link_libraries(logistics_vision_node PRIVATE logistics::vision_mqtt_workflow)

if(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT)
    target_compile_definitions(logistics_vision_node PRIVATE LOGISTICS_VISION_MQTT_ENABLED=1)
    target_link_libraries(
        logistics_vision_node
        PRIVATE logistics::device_mqtt_mosquitto logistics::device_status logistics::device_image_upload
    )
endif()
