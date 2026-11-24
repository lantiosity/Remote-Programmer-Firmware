#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_cfg.h"
#include "esp_swdlink_image.h"
#include "link.h"
#include "probe_core.h"
#include "target_flash_h743.h"

/* STM32 RM0433 FLASH registers and Cortex-M7 CoreDebug registers. */
enum {
    DBGMCU_IDCODE = 0x5c001000, FLASH_SIZE_REG = 0x1ff1e880,
    FLASH_REGS = 0x52002000, FLASH_BANK_STRIDE = 0x100,
    KEYR = 0x04, CR = 0x0c, SR = 0x10, CCR = 0x14,
    OPTCR = 0x18, OPTSR = 0x1c, WPSN_CUR = 0x38,
    BANK_SIZE = 0x100000, FLASH_WORD = 32,
};
#define DHCSR UINT32_C(0xe000edf0)
#define DEMCR UINT32_C(0xe000edfc)
#define DBGKEY UINT32_C(0xa05f0000)
#define C_DEBUGEN (1u << 0)
#define S_HALT (1u << 17)
#define VC_CORERESET (1u << 0)
#define CR_LOCK (1u << 0)
#define CR_PG (1u << 1)
#define CR_SER (1u << 2)
#define CR_PSIZE_32 (2u << 4)
#define CR_START (1u << 7)
#define SR_PENDING 7u /* BSY, WBNE, QW */
#define SR_ERROR UINT32_C(0x17ee0000)
#define SR_EOP (1u << 16)

bool target_flash_h743_range(uint32_t address, size_t size)
{
    return size && address >= H743_FLASH_BASE &&
        address < H743_FLASH_BASE + H743_FLASH_SIZE &&
        !(address % H743_SECTOR_SIZE) && size <= H743_FLASH_BASE + H743_FLASH_SIZE - address;
}

static bool read32(uint32_t addr, uint32_t *out)
{
    return probe_mem_read(addr, 32, out) == PROBE_OK;
}

static bool write32(uint32_t addr, uint32_t value)
{
    return probe_mem_write(addr, 32, value) == PROBE_OK && probe_mem_flush() == PROBE_OK;
}

static esp_err_t wait_flash(uint32_t bank, uint32_t timeout_ms, bool clear_old)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (unsigned polls = 0; ; ++polls) {
        uint32_t sr;
        if (!read32(bank + SR, &sr)) return ESP_FAIL;
        if (!(sr & SR_PENDING)) {
            if (sr & (SR_ERROR | SR_EOP)) {
                if (!write32(bank + CCR, sr & (SR_ERROR | SR_EOP))) return ESP_FAIL;
            }
            return !clear_old && (sr & SR_ERROR) ? ESP_FAIL : ESP_OK;
        }
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        if (polls >= 4) vTaskDelay(1);
    }
}

static esp_err_t unlock_bank(uint32_t bank)
{
    esp_err_t err = wait_flash(bank, 1000, true);
    if (err != ESP_OK) return err;
    uint32_t cr;
    if (!read32(bank + CR, &cr)) return ESP_FAIL;
    if (!(cr & CR_LOCK)) return ESP_OK;
    if (!write32(bank + KEYR, 0x45670123) || !write32(bank + KEYR, 0xcdef89ab) ||
        !read32(bank + CR, &cr)) return ESP_FAIL;
    return cr & CR_LOCK ? ESP_ERR_NOT_ALLOWED : ESP_OK;
}

static esp_err_t program_sector(uint32_t addr, const uint8_t *image, size_t size,
                                size_t offset, size_t total, flash_progress_t report)
{
    uint32_t index = (addr - H743_FLASH_BASE) / H743_SECTOR_SIZE;
    uint32_t bank = FLASH_REGS + (index / 8) * FLASH_BANK_STRIDE;
    report("erase", offset, total);
    esp_err_t err = unlock_bank(bank);
    if (err != ESP_OK) return err;
    const uint32_t erase = CR_SER | CR_PSIZE_32 | ((index % 8) << 8);
    if (!write32(bank + CR, erase) || !write32(bank + CR, erase | CR_START)) {
        err = ESP_FAIL;
        goto lock;
    }
    err = wait_flash(bank, 10000, false);
    if (err != ESP_OK) goto lock;
    if (!write32(bank + CR, CR_PG | CR_PSIZE_32)) {
        err = ESP_FAIL;
        goto lock;
    }
    report("program", offset, total);
    for (size_t pos = 0; pos < size; pos += FLASH_WORD) {
        uint32_t words[FLASH_WORD / 4];
        size_t count = size - pos < FLASH_WORD ? size - pos : FLASH_WORD;
        memset(words, 0xff, sizeof(words));
        memcpy(words, image + pos, count);
        unsigned i = 0;
        while (i < FLASH_WORD / 4 && words[i] == UINT32_MAX) ++i;
        if (i < FLASH_WORD / 4) {
            if (probe_mem_write_block32(addr + pos, words, FLASH_WORD / 4) != PROBE_OK) {
                err = ESP_FAIL;
                goto lock;
            }
            err = wait_flash(bank, 1000, false);
            if (err != ESP_OK) goto lock;
        }
        if (pos % 1024 == 0) {
            report("program", offset + pos, total);
            vTaskDelay(1);
        }
    }
lock:
    if (!write32(bank + CR, CR_LOCK) && err == ESP_OK) err = ESP_FAIL;
    return err;
}

static esp_err_t verify_image(uint32_t addr, const uint8_t *image, size_t size, flash_progress_t report)
{
    uint32_t buffer[256];
    for (size_t pos = 0; pos < size; pos += sizeof(buffer)) {
        size_t count = size - pos < sizeof(buffer) ? size - pos : sizeof(buffer);
        report("verify", pos, size);
        if (probe_mem_read_block32(addr + pos, buffer, (count + 3) / 4) != PROBE_OK ||
            memcmp(buffer, image + pos, count)) return ESP_ERR_INVALID_CRC;
        vTaskDelay(1);
    }
    return ESP_OK;
}

esp_err_t target_flash_h743(uint32_t addr, const uint8_t *image, size_t size,
                            bool reset_run, flash_progress_t report)
{
    if (!image || !report || !target_flash_h743_range(addr, size) || cfg_probe()->nreset_gpio < 0)
        return ESP_ERR_INVALID_ARG;
    if (esp_swdlink_image(image, size)) return ESP_ERR_INVALID_VERSION;
    probe_lock();
    if (link_swd_session_active()) {
        probe_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    report("connect", 0, size);
    const uint32_t old_clock = probe_get_clock();
    probe_set_clock(4000000);
    esp_err_t err = ESP_FAIL;
    uint32_t id, capacity, options, swap, dhcsr, saved_demcr;
    bool catch_reset = false, erase_started = false;

    probe_assert_reset(true);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (probe_connect() != ESP_OK || !probe_is_connected() || probe_mem_select_ap(0, 0) != PROBE_OK ||
        !read32(DBGMCU_IDCODE, &id)) goto end;
    if ((id & 0xfff) != 0x450) { err = ESP_ERR_NOT_SUPPORTED; goto end; }
    if (!read32(DEMCR, &saved_demcr) || !write32(DHCSR, DBGKEY | C_DEBUGEN)) goto end;
    if (!write32(DEMCR, saved_demcr | VC_CORERESET)) goto end;
    catch_reset = true;
    probe_assert_reset(false);
    vTaskDelay(pdMS_TO_TICKS(15));
    if (!read32(DHCSR, &dhcsr) || !(dhcsr & S_HALT) ||
        probe_mem_read(FLASH_SIZE_REG, 16, &capacity) != PROBE_OK) goto end;
    if (capacity != 2048) { err = ESP_ERR_NOT_SUPPORTED; goto end; }
    if (!read32(FLASH_REGS + OPTSR, &options) || !read32(FLASH_REGS + OPTCR, &swap)) goto end;
    if ((options & 0xff00u) != 0xaa00u || !(options & (1u << 4)) || (swap & (1u << 31))) {
        err = ESP_ERR_NOT_ALLOWED;
        goto end;
    }
    /* Check all sector protections before erasing the first sector. */
    const uint32_t first = (addr - H743_FLASH_BASE) / H743_SECTOR_SIZE;
    const uint32_t sectors = (size + H743_SECTOR_SIZE - 1) / H743_SECTOR_SIZE;
    for (uint32_t i = first; i < first + sectors; ++i) {
        uint32_t wrp;
        if (!read32(FLASH_REGS + (i / 8) * FLASH_BANK_STRIDE + WPSN_CUR, &wrp)) goto end;
        if (!(wrp & (1u << (i % 8)))) { err = ESP_ERR_NOT_ALLOWED; goto end; }
    }
    erase_started = true;
    for (size_t pos = 0; pos < size; pos += H743_SECTOR_SIZE) {
        size_t count = size - pos < H743_SECTOR_SIZE ? size - pos : H743_SECTOR_SIZE;
        err = program_sector(addr + pos, image + pos, count, pos, size, report);
        if (err != ESP_OK) goto end;
    }
    err = verify_image(addr, image, size, report);
end:
    if (catch_reset) {
        /* Restore vector catch. After a failed write the core stays halted. */
        if (!write32(DEMCR, saved_demcr) && err == ESP_OK) err = ESP_FAIL;
        if (!erase_started || (err == ESP_OK && reset_run)) {
            if (!write32(DHCSR, DBGKEY | C_DEBUGEN) && err == ESP_OK) err = ESP_FAIL;
            if (erase_started && err == ESP_OK && probe_reset_target() != PROBE_OK) err = ESP_FAIL;
        }
    }
    probe_assert_reset(false);
    probe_disconnect();
    probe_set_clock(old_clock);
    probe_unlock();
    return err;
}
