add_library(logistics_contracts INTERFACE)
add_library(logistics::contracts ALIAS logistics_contracts)

target_include_directories(
    logistics_contracts
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(BUILD_TESTING)
    add_executable(contracts_test tests/contracts_test.cpp)
    target_link_libraries(contracts_test PRIVATE logistics::contracts)
    add_test(NAME contracts_test COMMAND contracts_test)
endif()

