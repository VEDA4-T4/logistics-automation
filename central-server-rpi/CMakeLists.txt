add_executable(
    logistics_central_server
    src/main.cpp
    src/application.cpp
    src/device_manager/device_manager.cpp
)

target_include_directories(logistics_central_server PRIVATE include)
target_link_libraries(logistics_central_server PRIVATE logistics::contracts)

include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)
logistics_enable_warnings(logistics_central_server)

