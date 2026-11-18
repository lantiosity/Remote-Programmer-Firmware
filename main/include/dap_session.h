#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link.h"

#define DAP_IDLE_TIMEOUT_US INT64_C(120000000)

typedef enum {
    DAP_USE,
    DAP_OPEN,
    DAP_LOCK,
    DAP_UNLOCK,
    DAP_CLOSE,
} dap_access_t;

typedef struct {
    bool held;
    link_sink_t sink;
    int token;
    bool opened;
    unsigned lock_depth;
    int64_t last_activity;
} dap_session_t;

typedef struct {
    dap_session_t *session;
    link_sink_t sink;
    int token;
    bool used;
} dap_client_t;

bool dap_session_access(dap_client_t *client, dap_access_t access);
bool dap_session_expired(const dap_session_t *session, int token, int64_t now);
