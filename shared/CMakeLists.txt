add_library(logistics_contracts INTERFACE)
add_library(logistics::contracts ALIAS logistics_contracts)

target_include_directories(
    logistics_contracts
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

add_library(
    logistics_uart STATIC
    contracts/uart/uart_codec.c
    contracts/uart/uart_crc16.c
    contracts/uart/uart_parser.c
)
add_library(logistics::uart ALIAS logistics_uart)

target_link_libraries(logistics_uart PUBLIC logistics::contracts)

if(BUILD_TESTING)
    add_executable(contracts_test tests/contracts_test.cpp)
    target_link_libraries(contracts_test PRIVATE logistics::contracts)

    add_executable(uart_contracts_test tests/uart_contracts_test.cpp)
    target_link_libraries(uart_contracts_test PRIVATE logistics::uart)
    
    if(WIN32)
        get_filename_component(LOGISTICS_COMPILER_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        add_test(
            NAME contracts_test
            COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                    $<TARGET_FILE:contracts_test>
        )
        add_test(
            NAME uart_contracts_test
            COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                    $<TARGET_FILE:uart_contracts_test>
        )
    else()
        add_test(NAME contracts_test COMMAND contracts_test)
        add_test(NAME uart_contracts_test COMMAND uart_contracts_test)
    endif()
endif()
