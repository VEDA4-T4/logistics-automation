add_library(
    device_runtime
    common/src/mqtt_message_processor.cpp
    common/src/node_runtime.cpp
)
add_library(logistics::device_runtime ALIAS device_runtime)

target_include_directories(device_runtime PUBLIC common/include)
target_link_libraries(device_runtime PUBLIC logistics::contracts)

include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)
logistics_enable_warnings(device_runtime)

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
    target_link_libraries(device_mqtt_message_processor_test PRIVATE logistics::device_runtime)
    logistics_enable_warnings(device_mqtt_message_processor_test)
    add_test(NAME device_mqtt_message_processor_test COMMAND device_mqtt_message_processor_test)
endif()
