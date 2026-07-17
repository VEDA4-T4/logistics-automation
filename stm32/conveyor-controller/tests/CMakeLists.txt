add_executable(
    input_control_test
    input_control_test.c
    ../Application/Src/input_control.c
)

add_executable(
    input_motor_tb6612_test
    input_motor_tb6612_test.c
    ../Application/Src/input_motor_tb6612.c
)

target_include_directories(
    input_control_test
    PRIVATE ../Application/Inc
)

target_include_directories(
    input_motor_tb6612_test
    PRIVATE
        fakes
        ../Application/Inc
)

set_target_properties(
    input_control_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

set_target_properties(
    input_motor_tb6612_test
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

target_link_libraries(input_control_test PRIVATE logistics::contracts)
target_link_libraries(input_motor_tb6612_test PRIVATE logistics::contracts)

if(WIN32)
    get_filename_component(INPUT_CONTROL_COMPILER_RUNTIME_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    add_test(
        NAME input_control_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:input_control_test>
    )
    add_test(
        NAME input_motor_tb6612_test
        COMMAND ${CMAKE_COMMAND} -E env
                "PATH=${INPUT_CONTROL_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:input_motor_tb6612_test>
    )
else()
    add_test(NAME input_control_test COMMAND input_control_test)
    add_test(NAME input_motor_tb6612_test COMMAND input_motor_tb6612_test)
endif()
