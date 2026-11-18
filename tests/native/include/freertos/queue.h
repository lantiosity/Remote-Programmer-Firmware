#pragma once
#include "freertos/FreeRTOS.h"

typedef void *QueueHandle_t;
#ifdef WIRELINK_SERIAL_TEST
QueueHandle_t xQueueCreate(unsigned depth, unsigned size);
void vQueueDelete(QueueHandle_t queue);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
#else
static inline QueueHandle_t xQueueCreate(unsigned depth, unsigned size) { return NULL; }
static inline void vQueueDelete(QueueHandle_t queue) { }
static inline int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait) { return 0; }
static inline int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait) { return 0; }
#endif
