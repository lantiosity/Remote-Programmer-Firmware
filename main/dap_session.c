#include <limits.h>

#include "dap_session.h"

bool dap_session_access(dap_client_t *client, dap_access_t access)
{
    dap_session_t *session = client->session;
    if (session->held && (session->sink != client->sink || session->token != client->token)) {
        /* Remote close may clean the caller's handle table after a handoff. */
        return access == DAP_CLOSE;
    }
    client->used = true;

    if (access == DAP_CLOSE || access == DAP_DISCONNECT) {
        session->held = false;
        return true;
    }
    if (access == DAP_UNLOCK) {
        if (session->held && session->lock_depth && --session->lock_depth == 0 && !session->opened)
            session->held = false;
        return true;
    }
    if (!session->held) {
        *session = (dap_session_t){
            .held = true, .sink = client->sink, .token = client->token,
            .opened = access != DAP_LOCK,
        };
    }
    if (access == DAP_OPEN) session->opened = true;
    if (access == DAP_LOCK) {
        if (session->lock_depth == UINT_MAX) return false;
        ++session->lock_depth;
    }
    return true;
}

bool dap_session_expired(const dap_session_t *session, int token, int64_t now)
{
    return session->held && (token != session->token || now - session->last_activity >= DAP_IDLE_TIMEOUT_US);
}
