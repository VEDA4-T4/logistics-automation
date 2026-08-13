find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MICROHTTPD REQUIRED IMPORTED_TARGET libmicrohttpd)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectWarnings.cmake")

# ------------------------------------------------------------------------------
# Storage
# ------------------------------------------------------------------------------

add_library(
    logistics_central_storage
    src/db_manager/database.cpp
    src/db_manager/history_service.cpp
    src/http_upload/upload_service.cpp
    src/db_manager/persistence.cpp
    src/db_manager/process_state_store.cpp
    src/process_manager/work_invalidation.cpp
)
add_library(logistics::central_storage ALIAS logistics_central_storage)

target_include_directories(logistics_central_storage PUBLIC include)
target_link_libraries(
    logistics_central_storage
    PUBLIC
        logistics::contracts
        SQLite::SQLite3
        OpenSSL::Crypto
)

logistics_enable_warnings(logistics_central_storage)

add_library(
    logistics_central_http_upload_server
    src/http_upload/http_upload_server.cpp
)
add_library(logistics::central_http_upload_server ALIAS logistics_central_http_upload_server)
target_include_directories(logistics_central_http_upload_server PUBLIC include)
target_link_libraries(
    logistics_central_http_upload_server
    PUBLIC logistics::central_storage
    PRIVATE PkgConfig::MICROHTTPD
)
logistics_enable_warnings(logistics_central_http_upload_server)

# ------------------------------------------------------------------------------
# MQTT core
# ------------------------------------------------------------------------------

add_library(
    logistics_central_server_mqtt_core
    src/mqtt_broker/mqtt_config.cpp
    src/mqtt_broker/mqtt_client.cpp
)
add_library(
    logistics::central_server_mqtt_core
    ALIAS logistics_central_server_mqtt_core
)

target_include_directories(
    logistics_central_server_mqtt_core
    PUBLIC include
)
target_link_libraries(
    logistics_central_server_mqtt_core
    PUBLIC logistics::contracts
)

logistics_enable_warnings(logistics_central_server_mqtt_core)

# ------------------------------------------------------------------------------
# Device manager
# ------------------------------------------------------------------------------

add_library(
    logistics_central_server_device_manager
    src/device_manager/device_manager.cpp
)
add_library(
    logistics::central_server_device_manager
    ALIAS logistics_central_server_device_manager
)

target_include_directories(
    logistics_central_server_device_manager
    PUBLIC include
)
target_link_libraries(
    logistics_central_server_device_manager
    PUBLIC logistics::contracts
)

logistics_enable_warnings(logistics_central_server_device_manager)

# ------------------------------------------------------------------------------
# Command manager
# ------------------------------------------------------------------------------

add_library(
    logistics_central_server_command_manager
    src/command_manager/command_manager.cpp
)
add_library(
    logistics::central_server_command_manager
    ALIAS logistics_central_server_command_manager
)

target_include_directories(
    logistics_central_server_command_manager
    PUBLIC include
)
target_link_libraries(
    logistics_central_server_command_manager
    PUBLIC
        logistics::central_server_device_manager
        logistics::contracts
)

logistics_enable_warnings(logistics_central_server_command_manager)

# ------------------------------------------------------------------------------
# Process state machine
# ------------------------------------------------------------------------------

add_library(
    logistics_central_server_process_state_machine
    src/process_manager/process_state_machine.cpp
)
add_library(
    logistics::central_server_process_state_machine
    ALIAS logistics_central_server_process_state_machine
)
target_include_directories(
    logistics_central_server_process_state_machine
    PUBLIC include
)
target_link_libraries(
    logistics_central_server_process_state_machine
    PUBLIC logistics::contracts
)
logistics_enable_warnings(logistics_central_server_process_state_machine)
target_link_libraries(
    logistics_central_storage
    PUBLIC logistics::central_server_process_state_machine
)

add_library(
    logistics_central_server_process_orchestrator
    src/process_manager/homography.cpp
    src/process_manager/process_orchestrator.cpp
)
add_library(
    logistics::central_server_process_orchestrator
    ALIAS logistics_central_server_process_orchestrator
)
target_include_directories(
    logistics_central_server_process_orchestrator
    PUBLIC include
)
target_link_libraries(
    logistics_central_server_process_orchestrator
    PUBLIC
        logistics::central_server_process_state_machine
        logistics::contracts
)
logistics_enable_warnings(logistics_central_server_process_orchestrator)

# ------------------------------------------------------------------------------
# Server configuration
# ------------------------------------------------------------------------------

add_library(logistics_central_server_config src/server_config.cpp)
add_library(logistics::central_server_config ALIAS logistics_central_server_config)
target_include_directories(logistics_central_server_config PUBLIC include)
target_link_libraries(
    logistics_central_server_config
    PUBLIC logistics::central_server_process_orchestrator
)
logistics_enable_warnings(logistics_central_server_config)

# ------------------------------------------------------------------------------
# MQTT handler
# ------------------------------------------------------------------------------

add_library(
    logistics_central_server_mqtt_handler
    src/mqtt_handler/mqtt_handler.cpp
    src/mqtt_handler/sensor_detection.cpp
)
add_library(
    logistics::central_server_mqtt_handler
    ALIAS logistics_central_server_mqtt_handler
)

target_include_directories(
    logistics_central_server_mqtt_handler
    PUBLIC include
)
target_link_libraries(
    logistics_central_server_mqtt_handler
    PUBLIC
        logistics::central_server_device_manager
        logistics::central_storage
        logistics::contracts
)

logistics_enable_warnings(logistics_central_server_mqtt_handler)

# ------------------------------------------------------------------------------
# Mosquitto transport and central server executable
# ------------------------------------------------------------------------------

if(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT)
    find_package(Threads REQUIRED)
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        pkg_check_modules(
            MOSQUITTO
            QUIET
            IMPORTED_TARGET
            libmosquitto
        )
    endif()

    if(TARGET PkgConfig::MOSQUITTO)
        set(LOGISTICS_MOSQUITTO_TARGET PkgConfig::MOSQUITTO)
    else()
        find_path(
            MOSQUITTO_INCLUDE_DIR
            mosquitto.h
        )
        find_library(
            MOSQUITTO_LIBRARY
            NAMES mosquitto
        )

        if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
            add_library(
                Mosquitto::Mosquitto
                UNKNOWN
                IMPORTED
            )
            set_target_properties(
                Mosquitto::Mosquitto
                PROPERTIES
                    IMPORTED_LOCATION "${MOSQUITTO_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${MOSQUITTO_INCLUDE_DIR}"
            )
            set(LOGISTICS_MOSQUITTO_TARGET Mosquitto::Mosquitto)
        endif()
    endif()

    if(NOT LOGISTICS_MOSQUITTO_TARGET)
        message(
            FATAL_ERROR
            "libmosquitto was not found. Install libmosquitto-dev on "
            "Ubuntu/Raspberry Pi, or configure with "
            "-DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=OFF for core-only tests."
        )
    endif()

    add_library(
        logistics_central_server_mosquitto
        src/mqtt_broker/mosquitto_transport.cpp
    )
    add_library(
        logistics::central_server_mosquitto
        ALIAS logistics_central_server_mosquitto
    )

    target_include_directories(
        logistics_central_server_mosquitto
        PUBLIC include
    )
    target_link_libraries(
        logistics_central_server_mosquitto
        PUBLIC
            logistics::central_server_mqtt_core
        PRIVATE
            ${LOGISTICS_MOSQUITTO_TARGET}
    )

    logistics_enable_warnings(logistics_central_server_mosquitto)

    add_executable(
        logistics_central_server
        src/main.cpp
        src/application.cpp
    )

    target_include_directories(
        logistics_central_server
        PRIVATE include
    )
    target_link_libraries(
        logistics_central_server
        PRIVATE
            logistics::central_storage
            logistics::central_http_upload_server
            logistics::central_server_command_manager
            logistics::central_server_process_orchestrator
            logistics::central_server_config
            logistics::central_server_mosquitto
            logistics::central_server_mqtt_handler
            Threads::Threads
    )

    logistics_enable_warnings(logistics_central_server)

    add_executable(
        central_server_mqtt_smoke
        tests/mqtt_publish_smoke.cpp
    )

    target_include_directories(
        central_server_mqtt_smoke
        PRIVATE include
    )
    target_link_libraries(
        central_server_mqtt_smoke
        PRIVATE
            logistics::central_server_mosquitto
            logistics::central_server_mqtt_core
            logistics::contracts
            Threads::Threads
    )

    logistics_enable_warnings(central_server_mqtt_smoke)

    install(
        TARGETS logistics_central_server
        RUNTIME DESTINATION bin
    )
    install(
        DIRECTORY db/migrations/
        DESTINATION share/logistics/migrations
        FILES_MATCHING PATTERN "*.sql"
    )
    if(UNIX)
        install(
            FILES "${CMAKE_CURRENT_LIST_DIR}/../deploy/systemd/logistics-central-server.service"
            DESTINATION lib/systemd/system
        )
    endif()
endif()

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------

if(BUILD_TESTING)
    add_executable(
        central_server_mqtt_test
        tests/mqtt_client_test.cpp
    )
    target_link_libraries(
        central_server_mqtt_test
        PRIVATE logistics::central_server_mqtt_core
    )
    logistics_enable_warnings(central_server_mqtt_test)
    add_test(
        NAME central_server_mqtt_test
        COMMAND central_server_mqtt_test
    )

    add_executable(central_server_config_test tests/server_config_test.cpp)
    target_link_libraries(central_server_config_test PRIVATE logistics::central_server_config)
    logistics_enable_warnings(central_server_config_test)
    add_test(NAME central_server_config_test COMMAND central_server_config_test)

    add_executable(
        central_server_device_manager_test
        tests/device_manager_test.cpp
    )
    target_link_libraries(
        central_server_device_manager_test
        PRIVATE logistics::central_server_device_manager
    )
    logistics_enable_warnings(central_server_device_manager_test)
    add_test(
        NAME central_server_device_manager_test
        COMMAND central_server_device_manager_test
    )

    add_executable(
        central_server_command_manager_test
        tests/command_manager_test.cpp
    )
    target_link_libraries(
        central_server_command_manager_test
        PRIVATE logistics::central_server_command_manager
    )
    logistics_enable_warnings(central_server_command_manager_test)
    add_test(
        NAME central_server_command_manager_test
        COMMAND central_server_command_manager_test
    )

    add_executable(
        central_server_process_state_machine_test
        tests/process_state_machine_test.cpp
    )
    target_link_libraries(
        central_server_process_state_machine_test
        PRIVATE logistics::central_server_process_state_machine
    )
    logistics_enable_warnings(central_server_process_state_machine_test)
    add_test(
        NAME central_server_process_state_machine_test
        COMMAND central_server_process_state_machine_test
    )

    add_executable(central_server_process_state_store_test tests/process_state_store_test.cpp)
    target_link_libraries(
        central_server_process_state_store_test
        PRIVATE
            logistics::central_storage
            logistics::central_server_command_manager
            logistics::central_server_process_state_machine
    )
    target_compile_definitions(
        central_server_process_state_store_test
        PRIVATE LOGISTICS_TEST_MIGRATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/db/migrations"
    )
    logistics_enable_warnings(central_server_process_state_store_test)
    add_test(NAME central_server_process_state_store_test COMMAND central_server_process_state_store_test)

    add_executable(
        central_server_process_orchestrator_test
        tests/process_orchestrator_test.cpp
    )
    target_link_libraries(
        central_server_process_orchestrator_test
        PRIVATE logistics::central_server_process_orchestrator
    )
    logistics_enable_warnings(central_server_process_orchestrator_test)
    add_test(
        NAME central_server_process_orchestrator_test
        COMMAND central_server_process_orchestrator_test
    )

    add_executable(
        central_server_process_integration_test
        tests/process_integration_test.cpp
    )
    target_link_libraries(
        central_server_process_integration_test
        PRIVATE
            logistics::central_server_command_manager
            logistics::central_server_mqtt_handler
            logistics::central_server_process_orchestrator
    )
    target_compile_definitions(
        central_server_process_integration_test
        PRIVATE LOGISTICS_TEST_MIGRATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/db/migrations"
    )
    logistics_enable_warnings(central_server_process_integration_test)
    add_test(NAME central_server_process_integration_test COMMAND central_server_process_integration_test)

    add_executable(central_server_homography_test tests/homography_test.cpp)
    target_link_libraries(
        central_server_homography_test
        PRIVATE logistics::central_server_process_orchestrator
    )
    logistics_enable_warnings(central_server_homography_test)
    add_test(NAME central_server_homography_test COMMAND central_server_homography_test)

    add_executable(
        central_server_sensor_detection_test
        tests/sensor_detection_test.cpp
    )
    target_link_libraries(
        central_server_sensor_detection_test
        PRIVATE logistics::central_server_mqtt_handler
    )
    logistics_enable_warnings(central_server_sensor_detection_test)
    add_test(
        NAME central_server_sensor_detection_test
        COMMAND central_server_sensor_detection_test
    )

    add_executable(
        central_server_mqtt_handler_test
        tests/mqtt_handler_test.cpp
    )
    target_link_libraries(
        central_server_mqtt_handler_test
        PRIVATE logistics::central_server_mqtt_handler
    )
    target_compile_definitions(
        central_server_mqtt_handler_test
        PRIVATE LOGISTICS_TEST_MIGRATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/db/migrations"
    )
    logistics_enable_warnings(central_server_mqtt_handler_test)
    add_test(
        NAME central_server_mqtt_handler_test
        COMMAND central_server_mqtt_handler_test
    )

    add_executable(
        central_storage_test
        tests/storage_test.cpp
    )
    target_link_libraries(
        central_storage_test
        PRIVATE logistics::central_storage
    )
    target_compile_definitions(
        central_storage_test
        PRIVATE
            LOGISTICS_TEST_MIGRATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/db/migrations"
    )
    logistics_enable_warnings(central_storage_test)

    if(WIN32)
        get_filename_component(
            LOGISTICS_COMPILER_RUNTIME_DIR
            "${CMAKE_CXX_COMPILER}"
            DIRECTORY
        )
        add_test(
            NAME central_storage_test
            COMMAND
                ${CMAKE_COMMAND} -E env
                "PATH=${LOGISTICS_COMPILER_RUNTIME_DIR};$ENV{PATH}"
                $<TARGET_FILE:central_storage_test>
        )
    else()
        add_test(
            NAME central_storage_test
            COMMAND central_storage_test
        )
    endif()

    add_executable(central_upload_service_test tests/upload_service_test.cpp)
    target_link_libraries(central_upload_service_test PRIVATE logistics::central_storage)
    target_compile_definitions(
        central_upload_service_test
        PRIVATE LOGISTICS_TEST_MIGRATION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/db/migrations"
    )
    logistics_enable_warnings(central_upload_service_test)
    add_test(NAME central_upload_service_test COMMAND central_upload_service_test)

    if(UNIX)
        add_executable(central_http_upload_server_test tests/http_upload_server_test.cpp)
        target_link_libraries(central_http_upload_server_test PRIVATE logistics::central_http_upload_server)
        logistics_enable_warnings(central_http_upload_server_test)
        add_test(NAME central_http_upload_server_test COMMAND central_http_upload_server_test)
    endif()
endif()
