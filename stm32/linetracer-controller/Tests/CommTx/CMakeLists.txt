add_executable(
    linetracer_comm_tx_logic_test
    comm_tx_logic_test.cpp
    ../../Application/Src/comm_tx_logic.c
)

target_include_directories(
    linetracer_comm_tx_logic_test
    PRIVATE ../../Application/Inc
)

target_link_libraries(linetracer_comm_tx_logic_test PRIVATE logistics::uart)

if(MSVC)
    target_compile_options(linetracer_comm_tx_logic_test PRIVATE /W4 /WX)
else()
    target_compile_options(linetracer_comm_tx_logic_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

if(WIN32)
    get_filename_component(LOGISTICS_COMM_TX_TEST_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_test(
        NAME linetracer_comm_tx_logic_test
        COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_COMM_TX_TEST_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:linetracer_comm_tx_logic_test>
    )
else()
    add_test(NAME linetracer_comm_tx_logic_test COMMAND linetracer_comm_tx_logic_test)
endif()

add_executable(
    linetracer_comm_tx_integration_test
    comm_tx_integration_test.cpp
    ../../Application/Src/comm_tx_logic.c
    ../../Application/Src/control_logic.c
    ../../Application/Src/route_planner.c
)

target_include_directories(
    linetracer_comm_tx_integration_test
    PRIVATE ../../Application/Inc
)

target_link_libraries(
    linetracer_comm_tx_integration_test
    PRIVATE logistics::uart logistics::contracts
)

if(MSVC)
    target_compile_options(linetracer_comm_tx_integration_test PRIVATE /W4 /WX)
else()
    target_compile_options(linetracer_comm_tx_integration_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

if(WIN32)
    add_test(
        NAME linetracer_comm_tx_integration_test
        COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_COMM_TX_TEST_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:linetracer_comm_tx_integration_test>
    )
else()
    add_test(NAME linetracer_comm_tx_integration_test COMMAND linetracer_comm_tx_integration_test)
endif()
