#pragma once
#include <stdlib.h>
typedef void *SemaphoreHandle_t;
extern unsigned mock_lock_errors, mock_locks_held;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(unsigned)); }
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return xSemaphoreCreateMutex(); }
static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned t) {
    if (!s || *(unsigned *)s) { ++mock_lock_errors; return 0; }
    ++*(unsigned *)s; ++mock_locks_held; return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s || !*(unsigned *)s) { ++mock_lock_errors; return 0; }
    --*(unsigned *)s; --mock_locks_held; return 1;
}
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t s, unsigned t) {
    ++*(unsigned *)s; ++mock_locks_held; return 1;
}
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return xSemaphoreGive(s); }
