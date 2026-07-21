cmake_minimum_required(VERSION 3.16)

project(linetracer_sensor_logic_tests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(sensor_logic_test
    sensor_logic_test.cpp
    ../../Application/Src/sensor_logic.c
)

target_include_directories(sensor_logic_test PRIVATE
    ../../Application/Inc
    ../../../../shared/include
)

if(MSVC)
    target_compile_options(sensor_logic_test PRIVATE /W4 /WX)
else()
    target_compile_options(sensor_logic_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

enable_testing()
add_test(NAME sensor_logic_test COMMAND sensor_logic_test)
