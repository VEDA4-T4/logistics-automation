add_library(logistics_contracts INTERFACE)
add_library(logistics::contracts ALIAS logistics_contracts)

if(WIN32 AND (NOT DEFINED nlohmann_json_DIR OR NOT nlohmann_json_DIR))
    if(DEFINED VCPKG_TARGET_TRIPLET)
        set(LOGISTICS_VCPKG_TRIPLET "${VCPKG_TARGET_TRIPLET}")
    else()
        set(LOGISTICS_VCPKG_TRIPLET "x64-windows")
    endif()

    set(
        LOGISTICS_NLOHMANN_JSON_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../vcpkg_installed/${LOGISTICS_VCPKG_TRIPLET}/share/nlohmann_json"
    )
    if(EXISTS "${LOGISTICS_NLOHMANN_JSON_DIR}/nlohmann_jsonConfig.cmake")
        set(nlohmann_json_DIR "${LOGISTICS_NLOHMANN_JSON_DIR}")
    endif()
endif()

find_package(nlohmann_json 3.11 QUIET)

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(logistics_contracts INTERFACE nlohmann_json::nlohmann_json)
else()
    include(CheckIncludeFileCXX)
    check_include_file_cxx(nlohmann/json.hpp LOGISTICS_HAVE_NLOHMANN_JSON)

    if(NOT LOGISTICS_HAVE_NLOHMANN_JSON)
        if(WIN32)
            message(
                FATAL_ERROR
                    "nlohmann/json.hpp was not found. Install the vcpkg manifest dependencies from the project root."
            )
        else()
            message(
                FATAL_ERROR
                    "nlohmann/json.hpp was not found. Install nlohmann-json3-dev on Ubuntu/Raspberry Pi."
            )
        endif()
    endif()
endif()

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
        get_filename_component(
            LOGISTICS_COMPILER_RUNTIME_DIR
            "${CMAKE_CXX_COMPILER}"
            DIRECTORY
        )

        add_test(
            NAME contracts_test
            COMMAND ${CMAKE_COMMAND} -E env
                    "PATH=${LOGISTICS_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                    $<TARGET_FILE:contracts_test>
        )

        add_test(
            NAME uart_contracts_test
            COMMAND ${CMAKE_COMMAND} -E env
                    "PATH=${LOGISTICS_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                    $<TARGET_FILE:uart_contracts_test>
        )
    else()
        add_test(NAME contracts_test COMMAND contracts_test)
        add_test(NAME uart_contracts_test COMMAND uart_contracts_test)
    endif()
endif()