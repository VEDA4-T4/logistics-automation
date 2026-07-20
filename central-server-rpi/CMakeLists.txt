find_package(SQLite3 REQUIRED)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectWarnings.cmake")

# ------------------------------------------------------------------------------
# Storage
# ------------------------------------------------------------------------------

add_library(
    logistics_central_storage
    src/db_manager/database.cpp
    src/db_manager/persistence.cpp
)
add_library(logistics::central_storage ALIAS logistics_central_storage)

target_include_directories(logistics_central_storage PUBLIC include)
target_link_libraries(
    logistics_central_storage
    PUBLIC
        logistics::contracts
        SQLite::SQLite3
)

logistics_enable_warnings(logistics_central_storage)

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
# MQTT handler
# ------------------------------------------------------------------------------

add_library(
    logistics_central_server_mqtt_handler
    src/mqtt_handler/mqtt_handler.cpp
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
        central_server_mqtt_handler_test
        tests/mqtt_handler_test.cpp
    )
    target_link_libraries(
        central_server_mqtt_handler_test
        PRIVATE logistics::central_server_mqtt_handler
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
endif()