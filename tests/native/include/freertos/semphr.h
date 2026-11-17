#pragma once
#include <stdlib.h>
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (void *)1; }
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t s, unsigned t) { return 1; }
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return 1; }
