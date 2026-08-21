add_executable(
    linetracer_health_logic_test
    health_logic_test.cpp
    ../../Application/Src/health_logic.c
)

target_include_directories(
    linetracer_health_logic_test
    PRIVATE ../../Application/Inc
)

target_link_libraries(
    linetracer_health_logic_test
    PRIVATE logistics::contracts
)

if(MSVC)
    target_compile_options(linetracer_health_logic_test PRIVATE /W4 /WX)
else()
    target_compile_options(linetracer_health_logic_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

if(WIN32)
    get_filename_component(LOGISTICS_HEALTH_TEST_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_test(
        NAME linetracer_health_logic_test
        COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_HEALTH_TEST_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:linetracer_health_logic_test>
    )
else()
    add_test(NAME linetracer_health_logic_test COMMAND linetracer_health_logic_test)
endif()
