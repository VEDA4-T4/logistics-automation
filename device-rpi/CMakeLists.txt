add_library(device_runtime common/src/node_runtime.cpp)
add_library(logistics::device_runtime ALIAS device_runtime)

target_include_directories(device_runtime PUBLIC common/include)
target_link_libraries(device_runtime PUBLIC logistics::contracts)

function(add_device_node target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE logistics::device_runtime)

    include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)
    logistics_enable_warnings(${target_name})
endfunction()

add_device_node(logistics_input_node input-node/main.cpp)
add_device_node(logistics_vision_node vision-node/main.cpp)
add_device_node(logistics_sorting_node sorting-node/main.cpp)
add_device_node(logistics_linetracer_node linetracer-node/main.cpp)

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

find_package(OpenCV 4.10.0 EXACT REQUIRED COMPONENTS core highgui imgproc objdetect videoio)
target_include_directories(logistics_vision_node PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(logistics_vision_node PRIVATE ${OpenCV_LIBS})
