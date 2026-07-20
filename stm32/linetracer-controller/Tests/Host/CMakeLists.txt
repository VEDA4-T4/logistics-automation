add_executable(
    linetracer_control_logic_test
    control_logic_test.cpp
    ../../Application/Src/control_logic.c
)

target_include_directories(
    linetracer_control_logic_test
    PRIVATE ../../Application/Inc
)

target_link_libraries(linetracer_control_logic_test PRIVATE logistics::contracts)

if(WIN32)
    get_filename_component(LOGISTICS_CONTROL_TEST_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_test(
        NAME linetracer_control_logic_test
        COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_CONTROL_TEST_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:linetracer_control_logic_test>
    )
else()
    add_test(NAME linetracer_control_logic_test COMMAND linetracer_control_logic_test)
endif()
