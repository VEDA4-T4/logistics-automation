find_package(SQLite3 REQUIRED)

add_library(
    logistics_central_storage
    src/db_manager/database.cpp
    src/db_manager/persistence.cpp
)

target_include_directories(logistics_central_storage PUBLIC include)
target_link_libraries(logistics_central_storage PUBLIC logistics::contracts SQLite::SQLite3)

add_executable(logistics_central_server src/main.cpp src/application.cpp src/device_manager/device_manager.cpp)

target_include_directories(logistics_central_server PRIVATE include)
target_link_libraries(logistics_central_server PRIVATE logistics_central_storage)

include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)
logistics_enable_warnings(logistics_central_server)
logistics_enable_warnings(logistics_central_storage)

install(TARGETS logistics_central_server RUNTIME DESTINATION bin)
install(DIRECTORY db/migrations/ DESTINATION share/logistics/migrations FILES_MATCHING PATTERN "*.sql")
