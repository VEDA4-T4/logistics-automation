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

add_test(NAME sensor_logic_test COMMAND sensor_logic_test)

add_executable(
    linetracer_unload_logic_test
    unload_logic_test.cpp
    ../../Application/Src/unload_logic.c
)

target_include_directories(
    linetracer_unload_logic_test
    PRIVATE ../../Application/Inc
)

target_link_libraries(linetracer_unload_logic_test PRIVATE logistics::contracts)

if(MSVC)
    target_compile_options(linetracer_unload_logic_test PRIVATE /W4 /WX)
else()
    target_compile_options(linetracer_unload_logic_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

if(WIN32)
    get_filename_component(LOGISTICS_UNLOAD_TEST_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_test(
        NAME linetracer_unload_logic_test
        COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_UNLOAD_TEST_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:linetracer_unload_logic_test>
    )
else()
    add_test(NAME linetracer_unload_logic_test COMMAND linetracer_unload_logic_test)
endif()

add_executable(
    linetracer_control_logic_test
    control_logic_test.cpp
    motor_control_logic_test.cpp
    ../../Application/Src/control_logic.c
    ../../Application/Src/motor_control_logic.c
    ../../Application/Src/route_planner.c
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

add_executable(
    linetracer_route_planner_test
    route_planner_test.cpp
    ../../Application/Src/route_planner.c
)

target_include_directories(
    linetracer_route_planner_test
    PRIVATE ../../Application/Inc
)

target_link_libraries(linetracer_route_planner_test PRIVATE logistics::contracts)

if(WIN32)
    get_filename_component(LOGISTICS_ROUTE_TEST_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_test(
        NAME linetracer_route_planner_test
        COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_ROUTE_TEST_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:linetracer_route_planner_test>
    )
else()
    add_test(NAME linetracer_route_planner_test COMMAND linetracer_route_planner_test)
endif()

add_executable(
    linetracer_safety_policy_test
    safety_policy_test.cpp
    ../../Application/Src/safety_policy.c
)

target_include_directories(
    linetracer_safety_policy_test
    PRIVATE ../../Application/Inc
)

target_link_libraries(linetracer_safety_policy_test PRIVATE logistics::contracts)

if(WIN32)
    get_filename_component(LOGISTICS_SAFETY_POLICY_TEST_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_test(
        NAME linetracer_safety_policy_test
        COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_SAFETY_POLICY_TEST_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:linetracer_safety_policy_test>
    )
else()
    add_test(NAME linetracer_safety_policy_test COMMAND linetracer_safety_policy_test)
endif()
