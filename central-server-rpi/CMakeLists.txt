find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MICROHTTPD REQUIRED IMPORTED_TARGET libmicrohttpd)

add_library(
    logistics_central_storage
    src/db_manager/database.cpp
    src/http_upload/upload_service.cpp
    src/db_manager/persistence.cpp
)

target_include_directories(logistics_central_storage PUBLIC include)
target_link_libraries(logistics_central_storage PUBLIC logistics::contracts SQLite::SQLite3 OpenSSL::Crypto)

add_executable(
    logistics_central_server
    src/main.cpp
    src/application.cpp
    src/device_manager/device_manager.cpp
    src/http_upload/http_upload_server.cpp
)

target_include_directories(logistics_central_server PRIVATE include)
target_link_libraries(logistics_central_server PRIVATE logistics_central_storage PkgConfig::MICROHTTPD)

include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)
logistics_enable_warnings(logistics_central_server)
logistics_enable_warnings(logistics_central_storage)

install(TARGETS logistics_central_server RUNTIME DESTINATION bin)
install(DIRECTORY db/migrations/ DESTINATION share/logistics/migrations FILES_MATCHING PATTERN "*.sql")

if(BUILD_TESTING)
    add_executable(central_storage_test tests/storage_test.cpp)
    target_link_libraries(central_storage_test PRIVATE logistics_central_storage)
    target_compile_definitions(
        central_storage_test
        PRIVATE LOGISTICS_TEST_MIGRATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/db/migrations"
    )
    if(WIN32)
        get_filename_component(LOGISTICS_COMPILER_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        add_test(
            NAME central_storage_test
            COMMAND ${CMAKE_COMMAND} -E env "PATH=${LOGISTICS_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                    $<TARGET_FILE:central_storage_test>
        )
    else()
        add_test(NAME central_storage_test COMMAND central_storage_test)
    endif()

    add_executable(central_upload_service_test tests/upload_service_test.cpp)
    target_link_libraries(central_upload_service_test PRIVATE logistics_central_storage)
    target_compile_definitions(
        central_upload_service_test
        PRIVATE LOGISTICS_TEST_MIGRATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/db/migrations"
    )
    add_test(NAME central_upload_service_test COMMAND central_upload_service_test)
endif()
