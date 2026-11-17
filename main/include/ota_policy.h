#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Shared by authorization checks and their status representation. */
static inline bool ota_window_open(int64_t now, int64_t deadline,
                                   unsigned allowed, unsigned requested)
{
    return now < deadline && (allowed & requested) != 0;
}
