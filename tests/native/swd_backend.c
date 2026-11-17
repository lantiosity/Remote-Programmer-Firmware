#include <string.h>
#include "probe_swd.h"

uint8_t model_memory[16384];
uint32_t model_csw = 0x23000052, model_tar, model_posted;
unsigned model_transfers, model_tar_writes, model_fault;
unsigned mock_lock_errors, mock_locks_held;

uint8_t probe_swd_transfer(uint8_t request, uint32_t *data)
{
    ++model_transfers;
    if (model_fault) return SWD_ACK_FAULT;
    unsigned addr = (request >> 1) & 12u;
    bool read = (request & 4) != 0;
    if (!(request & 2)) {
        if (read) *data = addr == 12 ? model_posted : 0xa0000000;
        return SWD_ACK_OK;
    }
    uint32_t value = 0;
    if (addr == 0) {
        if (read) value = model_csw;
        else model_csw = *data;
    } else if (addr == 4) {
        if (read) value = model_tar;
        else { model_tar = *data; ++model_tar_writes; }
    } else if (addr == 12) {
        unsigned bytes = 1u << (model_csw & 7u);
        unsigned lane = model_tar & 3u;
        for (unsigned i = 0; i < bytes; ++i) {
            if (read) value |= (uint32_t)model_memory[(model_tar + i) & 16383u] << ((lane + i) * 8);
            else model_memory[(model_tar + i) & 16383u] = (uint8_t)(*data >> ((lane + i) * 8));
        }
        model_tar = (model_tar & ~1023u) | ((model_tar + bytes) & 1023u);
    }
    if (read) { *data = model_posted; model_posted = value; }
    return SWD_ACK_OK;
}
