add_executable(
    gripper_control_test
    gripper_control_test.c
    ../Application/Src/gripper_control.c
)

target_include_directories(
    gripper_control_test
    PRIVATE
        ../Application/Inc
        ${PROJECT_SOURCE_DIR}/shared/include
)

target_compile_features(gripper_control_test PRIVATE c_std_11)

if(MSVC)
    target_compile_options(gripper_control_test PRIVATE /W4 /WX)
else()
    target_compile_options(gripper_control_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME gripper_control_test COMMAND gripper_control_test)
