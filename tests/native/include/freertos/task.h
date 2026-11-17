#pragma once
static inline void vTaskDelay(unsigned ticks) { }
static inline int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                              void *arg, unsigned priority, void *handle) { return 1; }
