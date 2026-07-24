cmake_minimum_required(VERSION 3.20)

project(logistics_automation VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOGISTICS_BUILD_CONTROL_CENTER "Build the Qt control center" OFF)
option(LOGISTICS_BUILD_CENTRAL_SERVER "Build the central Raspberry Pi server" ON)
option(LOGISTICS_BUILD_DEVICE_NODES "Build Raspberry Pi device nodes" ON)
option(LOGISTICS_BUILD_DEVICE_UART_TRANSPORT "Build the Raspberry Pi UART transport" ON)
option(LOGISTICS_BUILD_VISION_NODE "Build the Raspberry Pi vision node" ON)
option(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT "Build MQTT clients with libmosquitto" ON)

add_subdirectory(shared)

if(BUILD_TESTING)
    add_subdirectory(stm32/conveyor-controller/tests)
endif()

if(LOGISTICS_BUILD_CONTROL_CENTER)
    add_subdirectory(control-center)
endif()

if(LOGISTICS_BUILD_CENTRAL_SERVER)
    add_subdirectory(central-server-rpi)
endif()

if(LOGISTICS_BUILD_DEVICE_NODES OR LOGISTICS_BUILD_DEVICE_UART_TRANSPORT)
    add_subdirectory(device-rpi)
endif()

