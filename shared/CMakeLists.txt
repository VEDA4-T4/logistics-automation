add_library(logistics_contracts INTERFACE)
add_library(logistics::contracts ALIAS logistics_contracts)

find_package(nlohmann_json 3.11 QUIET)

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(logistics_contracts INTERFACE nlohmann_json::nlohmann_json)
else()
    include(CheckIncludeFileCXX)
    check_include_file_cxx(nlohmann/json.hpp LOGISTICS_HAVE_NLOHMANN_JSON)

    if(NOT LOGISTICS_HAVE_NLOHMANN_JSON)
        message(
            FATAL_ERROR
                "nlohmann/json.hpp was not found. Install nlohmann-json3-dev on Ubuntu/Raspberry Pi."
        )
    endif()
endif()

target_include_directories(
    logistics_contracts
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(BUILD_TESTING)
    add_executable(contracts_test tests/contracts_test.cpp)
    target_link_libraries(contracts_test PRIVATE logistics::contracts)
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectWarnings.cmake")
    logistics_enable_warnings(contracts_test)
    add_test(NAME contracts_test COMMAND contracts_test)
endif()

