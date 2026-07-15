add_library(
    logistics_central_server_mqtt_core
    src/mqtt_broker/mqtt_config.cpp
    src/mqtt_broker/mqtt_client.cpp
)
add_library(logistics::central_server_mqtt_core ALIAS logistics_central_server_mqtt_core)

target_include_directories(logistics_central_server_mqtt_core PUBLIC include)
target_link_libraries(logistics_central_server_mqtt_core PUBLIC logistics::contracts)

include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)
logistics_enable_warnings(logistics_central_server_mqtt_core)

add_library(logistics_central_server_device_manager src/device_manager/device_manager.cpp)
add_library(logistics::central_server_device_manager ALIAS logistics_central_server_device_manager)
target_include_directories(logistics_central_server_device_manager PUBLIC include)
target_link_libraries(logistics_central_server_device_manager PUBLIC logistics::contracts)
logistics_enable_warnings(logistics_central_server_device_manager)

if(LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT)
    find_package(Threads REQUIRED)
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        pkg_check_modules(MOSQUITTO QUIET IMPORTED_TARGET libmosquitto)
    endif()

    if(TARGET PkgConfig::MOSQUITTO)
        set(LOGISTICS_MOSQUITTO_TARGET PkgConfig::MOSQUITTO)
    else()
        find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
        find_library(MOSQUITTO_LIBRARY NAMES mosquitto)
        if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
            add_library(Mosquitto::Mosquitto UNKNOWN IMPORTED)
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
                "libmosquitto was not found. Install libmosquitto-dev on Ubuntu/Raspberry Pi, "
                "or configure with -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=OFF for core-only tests."
        )
    endif()

    add_library(logistics_central_server_mosquitto src/mqtt_broker/mosquitto_transport.cpp)
    add_library(logistics::central_server_mosquitto ALIAS logistics_central_server_mosquitto)
    target_include_directories(logistics_central_server_mosquitto PUBLIC include)
    target_link_libraries(
        logistics_central_server_mosquitto
        PUBLIC logistics::central_server_mqtt_core
        PRIVATE ${LOGISTICS_MOSQUITTO_TARGET}
    )
    logistics_enable_warnings(logistics_central_server_mosquitto)

    add_executable(
        logistics_central_server
        src/main.cpp
        src/application.cpp
    )
    target_include_directories(logistics_central_server PRIVATE include)
    target_link_libraries(
        logistics_central_server
        PRIVATE
            logistics::central_server_mosquitto
            logistics::central_server_device_manager
            logistics::contracts
            Threads::Threads
    )
    logistics_enable_warnings(logistics_central_server)
endif()

if(BUILD_TESTING)
    add_executable(central_server_mqtt_test tests/mqtt_client_test.cpp)
    target_link_libraries(central_server_mqtt_test PRIVATE logistics::central_server_mqtt_core)
    logistics_enable_warnings(central_server_mqtt_test)
    add_test(NAME central_server_mqtt_test COMMAND central_server_mqtt_test)

    add_executable(central_server_device_manager_test tests/device_manager_test.cpp)
    target_link_libraries(central_server_device_manager_test PRIVATE logistics::central_server_device_manager)
    logistics_enable_warnings(central_server_device_manager_test)
    add_test(NAME central_server_device_manager_test COMMAND central_server_device_manager_test)
endif()
