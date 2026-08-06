add_executable(sensor_logic_test
    sensor_logic_test.cpp
    ../../Application/Src/sensor_logic.c
)

target_include_directories(sensor_logic_test PRIVATE
    ../../Application/Inc
    ../../../../shared/include
)

if(MSVC)
    target_compile_options(sensor_logic_test PRIVATE /utf-8 /W4 /WX /wd4310)
else()
    target_compile_options(sensor_logic_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

add_test(NAME sensor_logic_test COMMAND sensor_logic_test)

add_executable(ultrasonic_filter_test
    ultrasonic_filter_test.cpp
    ../../Application/Src/sensor_logic.c
)

target_include_directories(ultrasonic_filter_test PRIVATE
    ../../Application/Inc
    ../../../../shared/include
)

if(MSVC)
    target_compile_options(ultrasonic_filter_test PRIVATE /utf-8 /W4 /WX /wd4310)
else()
    target_compile_options(ultrasonic_filter_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

add_test(NAME ultrasonic_filter_test COMMAND ultrasonic_filter_test)

add_executable(sensor_logic_route_test_mode_test
    sensor_logic_test.cpp
    ../../Application/Src/sensor_logic.c
)

target_include_directories(sensor_logic_route_test_mode_test PRIVATE
    ../../Application/Inc
    ../../../../shared/include
)

target_compile_definitions(sensor_logic_route_test_mode_test PRIVATE
    LINETRACER_ROUTE_TEST_MODE=1
)

if(MSVC)
    target_compile_options(sensor_logic_route_test_mode_test PRIVATE /utf-8 /W4 /WX /wd4310)
else()
    target_compile_options(sensor_logic_route_test_mode_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

add_test(NAME sensor_logic_route_test_mode_test COMMAND sensor_logic_route_test_mode_test)

add_executable(
    line_follow_pid_test
    line_follow_pid_test.cpp
    ../../Application/Src/line_follow_pid.c
)

target_include_directories(
    line_follow_pid_test
    PRIVATE
        ../../Application/Inc
        ../../../../shared/include
)

if(MSVC)
    target_compile_options(line_follow_pid_test PRIVATE /utf-8 /W4 /WX /wd4310)
else()
    target_compile_options(line_follow_pid_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

add_test(NAME line_follow_pid_test COMMAND line_follow_pid_test)

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
    linetracer_obstacle_resume_test
    obstacle_resume_test.cpp
    ../../Application/Src/control_logic.c
    ../../Application/Src/route_planner.c
)

target_include_directories(
    linetracer_obstacle_resume_test
    PRIVATE ../../Application/Inc
)

target_link_libraries(linetracer_obstacle_resume_test PRIVATE logistics::contracts)

if(WIN32)
    get_filename_component(LOGISTICS_OBSTACLE_TEST_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_test(
        NAME linetracer_obstacle_resume_test
        COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_OBSTACLE_TEST_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:linetracer_obstacle_resume_test>
    )
else()
    add_test(NAME linetracer_obstacle_resume_test COMMAND linetracer_obstacle_resume_test)
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
