add_library(
    device_mqtt_core
    common/src/mqtt_node_config.cpp
    common/src/mqtt_message_processor.cpp
    common/src/mqtt_time.cpp
)
add_library(logistics::device_mqtt_core ALIAS device_mqtt_core)

target_include_directories(device_mqtt_core PUBLIC common/include)
target_link_libraries(device_mqtt_core PUBLIC logistics::contracts)

include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)
logistics_enable_warnings(device_mqtt_core)

add_library(
    device_runtime
    common/src/node_runtime.cpp
)
add_library(logistics::device_runtime ALIAS device_runtime)

target_include_directories(device_runtime PUBLIC common/include)
target_link_libraries(device_runtime PUBLIC logistics::contracts PRIVATE logistics::device_mqtt_core)

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

    add_library(device_mqtt_mosquitto common/src/mqtt_node_client.cpp)
    add_library(logistics::device_mqtt_mosquitto ALIAS device_mqtt_mosquitto)
    target_include_directories(device_mqtt_mosquitto PUBLIC common/include)
    target_link_libraries(
        device_mqtt_mosquitto
        PUBLIC logistics::device_mqtt_core
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
endif()
