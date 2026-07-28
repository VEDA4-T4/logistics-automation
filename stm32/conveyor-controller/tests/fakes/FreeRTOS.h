#ifndef TEST_FAKES_FREERTOS_H
#define TEST_FAKES_FREERTOS_H

#include <stdint.h>

typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;

#define pdFALSE ((BaseType_t)0)
#define pdTRUE ((BaseType_t)1)
#define pdFAIL pdFALSE
#define pdPASS pdTRUE

#endif /* TEST_FAKES_FREERTOS_H */
