#include <string.h>
#include "probe_core.h"

unsigned test_count, test_apsel, test_calls, test_bits;
uint8_t test_data[8192];
const char *cfg_serial_number(void) { return "native-test"; }
probe_status_t probe_mem_select_ap(uint32_t ap, uint32_t bank)
{ test_apsel = ap; return PROBE_OK; }
probe_status_t probe_mem_write_block8(uint32_t addr, const uint8_t *data, uint32_t count)
{ ++test_calls; test_count = count; memcpy(test_data, data, count); return PROBE_OK; }
probe_status_t probe_mem_read_block8(uint32_t addr, uint8_t *data, uint32_t count)
{ ++test_calls; memset(data, 255, count); return PROBE_OK; }
probe_status_t probe_mem_read_block32(uint32_t addr, uint32_t *data, uint32_t count)
{ ++test_calls; memset(data, 255, count * 4); return PROBE_OK; }
void probe_swj_sequence(uint32_t count, const uint8_t *data)
{ ++test_calls; test_bits = count; memset(test_data, 0, sizeof(test_data)); memcpy(test_data, data, (count + 7) / 8); }
