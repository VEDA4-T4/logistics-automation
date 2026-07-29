cmake_minimum_required(VERSION 3.20)

project(logistics_automation VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOGISTICS_BUILD_CONTROL_CENTER "Build the Qt control center" OFF)
option(LOGISTICS_BUILD_CENTRAL_SERVER "Build the central Raspberry Pi server" ON)
option(LOGISTICS_BUILD_DEVICE_NODES "Build Raspberry Pi device nodes" ON)
option(LOGISTICS_BUILD_LINETRACER_UART_TEST "Build the Raspberry Pi line-tracer UART test tool" OFF)

add_subdirectory(shared)

if(LOGISTICS_BUILD_LINETRACER_UART_TEST)
    add_executable(
        logistics_linetracer_uart_test
        device-rpi/tools/linetracer-uart-test/main.cpp
    )
    target_link_libraries(logistics_linetracer_uart_test PRIVATE logistics::uart)

    include(cmake/ProjectWarnings.cmake)
    logistics_enable_warnings(logistics_linetracer_uart_test)
endif()

if(BUILD_TESTING)
    add_subdirectory(stm32/linetracer-controller/Tests/CommRx)
    add_subdirectory(stm32/linetracer-controller/Tests/CommTx)
    add_subdirectory(stm32/linetracer-controller/Tests/Health)
    add_subdirectory(stm32/linetracer-controller/Tests/Host)
endif()

if(LOGISTICS_BUILD_CONTROL_CENTER)
    add_subdirectory(control-center)
endif()

if(LOGISTICS_BUILD_CENTRAL_SERVER)
    add_subdirectory(central-server-rpi)
endif()

if(LOGISTICS_BUILD_DEVICE_NODES)
    add_subdirectory(device-rpi)
endif()

