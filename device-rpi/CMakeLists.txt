find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)

add_library(device_runtime common/src/log_spool_uploader.cpp common/src/node_runtime.cpp)
add_library(logistics::device_runtime ALIAS device_runtime)

target_include_directories(device_runtime PUBLIC common/include)
target_link_libraries(
    device_runtime
    PUBLIC logistics::contracts
    PRIVATE OpenSSL::Crypto PkgConfig::LIBCURL Threads::Threads
)

include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)
logistics_enable_warnings(device_runtime)

function(add_device_node target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE logistics::device_runtime)

    include(${PROJECT_SOURCE_DIR}/cmake/ProjectWarnings.cmake)
    logistics_enable_warnings(${target_name})
endfunction()

add_device_node(logistics_input_node input-node/main.cpp)
add_device_node(logistics_vision_node vision-node/main.cpp)
add_device_node(logistics_sorting_node sorting-node/main.cpp)
add_device_node(logistics_linetracer_node linetracer-node/main.cpp)

target_sources(
    logistics_vision_node
    PRIVATE
        vision-node/detection.cpp
        vision-node/detection.hpp
)

find_package(OpenCV 4.10.0 EXACT REQUIRED COMPONENTS core highgui imgproc objdetect videoio)
target_include_directories(logistics_vision_node PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(logistics_vision_node PRIVATE ${OpenCV_LIBS})

if(BUILD_TESTING)
    add_executable(device_log_spool_test tests/log_spool_uploader_test.cpp)
    target_link_libraries(device_log_spool_test PRIVATE logistics::device_runtime)
    add_test(NAME device_log_spool_test COMMAND device_log_spool_test)
endif()
