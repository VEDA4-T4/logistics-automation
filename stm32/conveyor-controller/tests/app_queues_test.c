#ifdef NDEBUG
#undef NDEBUG
#endif

#include "app_queues.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "app_messages.h"

#define EXPECTED_QUEUE_COUNT 4U

typedef struct {
    uint32_t depth;
    uint32_t itemSize;
    const char* name;
} queue_creation_t;

static queue_creation_t queueCreations[EXPECTED_QUEUE_COUNT];
static uint8_t queueObjects[EXPECTED_QUEUE_COUNT];
static uint32_t queueCreationCount;
static uint32_t failCreationIndex;

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size, const osMessageQueueAttr_t* attr) {
    uint32_t index;

    index = queueCreationCount;
    assert(index < EXPECTED_QUEUE_COUNT);
    queueCreations[index].depth = msg_count;
    queueCreations[index].itemSize = msg_size;
    queueCreations[index].name = (attr != NULL) ? attr->name : NULL;
    queueCreationCount++;

    if (failCreationIndex == (index + 1U)) {
        return NULL;
    }

    return &queueObjects[index];
}

static void reset_fake(void) {
    memset(queueCreations, 0, sizeof(queueCreations));
    memset(queueObjects, 0, sizeof(queueObjects));
    queueCreationCount = 0U;
    failCreationIndex = 0U;
}

static void assert_creation(uint32_t index, uint32_t depth, uint32_t itemSize, const char* name) {
    assert(queueCreations[index].depth == depth);
    assert(queueCreations[index].itemSize == itemSize);
    assert(queueCreations[index].name != NULL);
    assert(strcmp(queueCreations[index].name, name) == 0);
}

static void test_queue_schema(void) {
    reset_fake();

    assert(app_queues_init() == osOK);
    assert(queueCreationCount == EXPECTED_QUEUE_COUNT);
    assert_creation(0U, 8U, sizeof(uart_rx_chunk_t), "uartRxQueue");
    assert_creation(1U, 8U, sizeof(control_command_t), "inputControlQueue");
    assert_creation(2U, 8U, sizeof(control_command_t), "sortingControlQueue");
    assert_creation(3U, 4U, sizeof(control_command_t), "safetyCommandQueue");
}

static void test_any_allocation_failure_is_reported(void) {
    uint32_t index;

    for (index = 1U; index <= EXPECTED_QUEUE_COUNT; index++) {
        reset_fake();
        failCreationIndex = index;
        assert(app_queues_init() == osErrorNoMemory);
        assert(queueCreationCount == EXPECTED_QUEUE_COUNT);
    }
}

int main(void) {
    test_queue_schema();
    test_any_allocation_failure_is_reported();
    return 0;
}
