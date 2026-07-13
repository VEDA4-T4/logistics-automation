cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(logistics_control_center_project LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../shared"
        "${CMAKE_CURRENT_BINARY_DIR}/shared"
    )
endif()

find_package(Qt6 REQUIRED COMPONENTS Multimedia MultimediaWidgets Widgets)

qt_standard_project_setup()

qt_add_executable(
    logistics_control_center
    src/main.cpp
    src/main_window.cpp
    include/logistics/control_center/main_window.hpp
)

add_custom_target(
    logistics_control_center_config ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/config"
    COMMAND
        ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/config"
        "${CMAKE_CURRENT_BINARY_DIR}/config"
    COMMENT "Copying control-center configuration"
    VERBATIM
)
add_dependencies(logistics_control_center logistics_control_center_config)

target_include_directories(logistics_control_center PRIVATE include)
target_link_libraries(
    logistics_control_center
    PRIVATE
        Qt6::Multimedia
        Qt6::MultimediaWidgets
        Qt6::Widgets
        logistics::contracts
)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ProjectWarnings.cmake")
logistics_enable_warnings(logistics_control_center)
