#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
