#ifndef TEST_FAKES_QUEUE_H
#define TEST_FAKES_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"

typedef void* QueueHandle_t;
typedef void* QueueSetHandle_t;
typedef void* QueueSetMemberHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
QueueSetHandle_t xQueueCreateSet(UBaseType_t event_queue_length);
BaseType_t xQueueAddToSet(QueueSetMemberHandle_t member, QueueSetHandle_t queue_set);
BaseType_t xQueueRemoveFromSet(QueueSetMemberHandle_t member, QueueSetHandle_t queue_set);
QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t queue_set, TickType_t timeout);
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t timeout);
BaseType_t xQueuePeek(QueueHandle_t queue, void* item, TickType_t timeout);
void vQueueDelete(QueueHandle_t queue);

#endif /* TEST_FAKES_QUEUE_H */
