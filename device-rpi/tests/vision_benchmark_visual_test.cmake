if(NOT DEFINED BENCHMARK_EXECUTABLE OR NOT DEFINED TEST_DIRECTORY)
    message(FATAL_ERROR "BENCHMARK_EXECUTABLE and TEST_DIRECTORY are required")
endif()

file(REMOVE_RECURSE "${TEST_DIRECTORY}")
file(MAKE_DIRECTORY "${TEST_DIRECTORY}/images")
string(REPEAT "255 255 255\n" 4096 WHITE_PIXELS)
file(WRITE "${TEST_DIRECTORY}/images/sample.ppm" "P3\n64 64\n255\n${WHITE_PIXELS}")
file(WRITE "${TEST_DIRECTORY}/manifest.csv" "filename,ean13\nsample.ppm,5901234123457\n")

execute_process(
    COMMAND
        "${BENCHMARK_EXECUTABLE}"
        --dataset "${TEST_DIRECTORY}/images"
        --manifest "${TEST_DIRECTORY}/manifest.csv"
        --iterations 1
        --visual-output "${TEST_DIRECTORY}/visuals"
        --visual-limit 1
    RESULT_VARIABLE BENCHMARK_RESULT
    OUTPUT_VARIABLE BENCHMARK_OUTPUT
    ERROR_VARIABLE BENCHMARK_ERROR
)
if(NOT BENCHMARK_RESULT EQUAL 0)
    message(
        FATAL_ERROR
            "visual benchmark failed (${BENCHMARK_RESULT})\n${BENCHMARK_OUTPUT}\n${BENCHMARK_ERROR}"
    )
endif()

file(GLOB VISUAL_OUTPUTS "${TEST_DIRECTORY}/visuals/*-sr-comparison.png")
list(LENGTH VISUAL_OUTPUTS VISUAL_OUTPUT_COUNT)
if(NOT VISUAL_OUTPUT_COUNT EQUAL 1)
    message(FATAL_ERROR "expected one SR comparison image, found ${VISUAL_OUTPUT_COUNT}")
endif()
list(GET VISUAL_OUTPUTS 0 VISUAL_OUTPUT)
file(SIZE "${VISUAL_OUTPUT}" VISUAL_OUTPUT_SIZE)
if(VISUAL_OUTPUT_SIZE LESS 1)
    message(FATAL_ERROR "SR comparison image is empty")
endif()
