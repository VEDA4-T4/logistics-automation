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
        --warmup 1
        --profile baseline
        --output "${TEST_DIRECTORY}/benchmark.csv"
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

file(STRINGS "${TEST_DIRECTORY}/benchmark.csv" BENCHMARK_LINES)
list(LENGTH BENCHMARK_LINES BENCHMARK_LINE_COUNT)
if(NOT BENCHMARK_LINE_COUNT EQUAL 2)
    message(FATAL_ERROR "expected one header and one profile row, found ${BENCHMARK_LINE_COUNT}")
endif()
list(GET BENCHMARK_LINES 0 BENCHMARK_HEADER)
list(GET BENCHMARK_LINES 1 BENCHMARK_ROW)
foreach(REQUIRED_COLUMN
    p99_total_ms
    cpu_percent
    average_rss_kb
    peak_rss_kb
    throughput_change_percent
    rss_growth_kb
)
    string(FIND "${BENCHMARK_HEADER}" "${REQUIRED_COLUMN}" REQUIRED_COLUMN_POSITION)
    if(REQUIRED_COLUMN_POSITION EQUAL -1)
        message(FATAL_ERROR "benchmark CSV header is missing ${REQUIRED_COLUMN}: ${BENCHMARK_HEADER}")
    endif()
endforeach()

string(REPLACE "," ";" BENCHMARK_HEADER_FIELDS "${BENCHMARK_HEADER}")
string(REPLACE "," ";" BENCHMARK_ROW_FIELDS "${BENCHMARK_ROW}")
list(LENGTH BENCHMARK_HEADER_FIELDS BENCHMARK_HEADER_FIELD_COUNT)
list(LENGTH BENCHMARK_ROW_FIELDS BENCHMARK_ROW_FIELD_COUNT)
if(NOT BENCHMARK_ROW_FIELD_COUNT EQUAL BENCHMARK_HEADER_FIELD_COUNT)
    message(
        FATAL_ERROR
            "benchmark CSV field count mismatch: header=${BENCHMARK_HEADER_FIELD_COUNT}, row=${BENCHMARK_ROW_FIELD_COUNT}"
    )
endif()
list(GET BENCHMARK_ROW_FIELDS 1 BENCHMARK_SAMPLE_COUNT)
if(NOT BENCHMARK_SAMPLE_COUNT EQUAL 1)
    message(FATAL_ERROR "warm-up samples leaked into measured sample count: ${BENCHMARK_SAMPLE_COUNT}")
endif()
