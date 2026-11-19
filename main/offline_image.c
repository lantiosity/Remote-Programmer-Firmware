#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "offline_image.h"

enum { BLOCK_SIZE = 4096, SLOT_SIZE = BLOCK_SIZE + H743_FLASH_SIZE };
#define RECORD_MAGIC UINT32_C(0x31495753) /* SWI1 */

typedef struct {
    uint32_t generation, size;
    uint8_t sha256[32];
    char name[OFFLINE_NAME_SIZE];
} image_record_t;

_Static_assert(sizeof(image_record_t) + sizeof(uint32_t) <= BLOCK_SIZE, "Metadata fits one sector");

bool offline_sd_candidate(const char *name)
{
    size_t length = strnlen(name, OFFLINE_NAME_SIZE);
    return length > 4 && length < OFFLINE_NAME_SIZE &&
        strcasecmp(name + length - 4, ".bin") == 0 &&
        !strchr(name, '/') && !strchr(name, '\\');
}

static void sd_path(char path[sizeof(OFFLINE_SD_ROOT) + OFFLINE_NAME_SIZE], const char *name)
{
    snprintf(path, sizeof(OFFLINE_SD_ROOT) + OFFLINE_NAME_SIZE, "%s/%s", OFFLINE_SD_ROOT, name);
}

esp_err_t offline_sd_inspect(const char *name, offline_image_t *image)
{
    if (!offline_sd_candidate(name)) return ESP_ERR_INVALID_ARG;
    char path[sizeof(OFFLINE_SD_ROOT) + OFFLINE_NAME_SIZE];
    sd_path(path, name);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return ESP_ERR_NOT_FOUND;
    if (st.st_size <= 0 || st.st_size > H743_FLASH_SIZE) return ESP_ERR_INVALID_SIZE;
    offline_image_t found = {.size = st.st_size};
    strcpy(found.name, name);
    *image = found;
    return ESP_OK;
}

static const esp_partition_t *storage(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "offline");
    return part && !part->encrypted && part->erase_size == BLOCK_SIZE && part->size >= 2 * SLOT_SIZE
        ? part : NULL;
}

static esp_err_t read_record(const esp_partition_t *part, unsigned slot, image_record_t *record)
{
    uint32_t magic;
    const size_t base = slot * SLOT_SIZE;
    esp_err_t err = esp_partition_read(part, base, &magic, sizeof(magic));
    if (err != ESP_OK) return err;
    if (magic != RECORD_MAGIC) return ESP_ERR_NOT_FOUND;
    err = esp_partition_read(part, base + sizeof(magic), record, sizeof(*record));
    if (err != ESP_OK) return err;
    if (!record->size || record->size > H743_FLASH_SIZE ||
        !memchr(record->name, 0, sizeof(record->name)) || !offline_sd_candidate(record->name))
        return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

static esp_err_t latest_record(const esp_partition_t *part, image_record_t *record, unsigned *slot)
{
    image_record_t second;
    esp_err_t first_err = read_record(part, 0, record);
    esp_err_t second_err = read_record(part, 1, &second);
    if (first_err != ESP_OK && first_err != ESP_ERR_NOT_FOUND) return first_err;
    if (second_err != ESP_OK && second_err != ESP_ERR_NOT_FOUND) return second_err;
    if (first_err != ESP_OK && second_err != ESP_OK) return ESP_ERR_NOT_FOUND;
    *slot = second_err == ESP_OK &&
        (first_err != ESP_OK || (int32_t)(second.generation - record->generation) > 0);
    if (*slot) *record = second;
    return ESP_OK;
}

esp_err_t offline_saved_inspect(offline_image_t *image)
{
    const esp_partition_t *part = storage();
    if (!part) return ESP_ERR_NOT_SUPPORTED;
    image_record_t record;
    unsigned slot;
    esp_err_t err = latest_record(part, &record, &slot);
    if (err != ESP_OK) return err;
    *image = (offline_image_t){.size = record.size, .generation = record.generation, .saved = true};
    strcpy(image->name, record.name);
    return ESP_OK;
}

static esp_err_t load_sd(const offline_image_t *source, uint8_t *data, flash_progress_t report)
{
    if (!offline_sd_candidate(source->name)) return ESP_ERR_INVALID_ARG;
    char path[sizeof(OFFLINE_SD_ROOT) + OFFLINE_NAME_SIZE];
    sd_path(path, source->name);
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    struct stat st;
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != source->size) goto end;

    err = ESP_FAIL;
    for (size_t done = 0; done < source->size;) {
        size_t length = source->size - done < BLOCK_SIZE ? source->size - done : BLOCK_SIZE;
        if (fread(data + done, 1, length, file) != length) goto end;
        done += length;
        report("load", done, source->size);
    }
    if (fgetc(file) == EOF && !ferror(file)) err = ESP_OK;
end:
    fclose(file);
    return err;
}

static esp_err_t compute_digest(const uint8_t *data, size_t size, uint8_t digest[32])
{
    size_t written;
    return psa_hash_compute(PSA_ALG_SHA_256, data, size, digest, 32, &written) == PSA_SUCCESS &&
        written == 32 ? ESP_OK : ESP_FAIL;
}

static esp_err_t load_saved(const offline_image_t *source, uint8_t *data, flash_progress_t report)
{
    const esp_partition_t *part = storage();
    if (!part) return ESP_ERR_NOT_SUPPORTED;
    image_record_t record;
    unsigned slot;
    esp_err_t err = latest_record(part, &record, &slot);
    if (err != ESP_OK) return err;
    if (record.generation != source->generation || record.size != source->size ||
        strcmp(record.name, source->name)) return ESP_ERR_INVALID_STATE;
    const size_t base = slot * SLOT_SIZE + BLOCK_SIZE;
    for (size_t done = 0; done < source->size;) {
        size_t length = source->size - done < BLOCK_SIZE ? source->size - done : BLOCK_SIZE;
        err = esp_partition_read(part, base + done, data + done, length);
        if (err != ESP_OK) return err;
        done += length;
        report("load", done, source->size);
    }
    uint8_t digest[32];
    err = compute_digest(data, source->size, digest);
    if (err != ESP_OK) return err;
    return memcmp(digest, record.sha256, sizeof(digest)) ? ESP_ERR_INVALID_CRC : ESP_OK;
}

esp_err_t offline_image_load(const offline_image_t *source, uint8_t **out, flash_progress_t report)
{
    if (!source->size || source->size > H743_FLASH_SIZE) return ESP_ERR_INVALID_SIZE;
    uint8_t *data = heap_caps_malloc(source->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) return ESP_ERR_NO_MEM;
    report("load", 0, source->size);
    esp_err_t err = source->saved ? load_saved(source, data, report) : load_sd(source, data, report);
    if (err == ESP_OK) *out = data;
    else free(data);
    return err;
}

esp_err_t offline_image_save(const offline_image_t *source, const uint8_t *image, flash_progress_t report)
{
    if (source->saved || !offline_sd_candidate(source->name) ||
        !source->size || source->size > H743_FLASH_SIZE) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *part = storage();
    if (!part) return ESP_ERR_NOT_SUPPORTED;
    image_record_t latest, record = {.size = source->size, .generation = 1};
    unsigned slot = 0;
    esp_err_t err = latest_record(part, &latest, &slot);
    if (err == ESP_OK) {
        record.generation = latest.generation + 1;
        slot ^= 1;
    } else if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }
    strcpy(record.name, source->name);
    err = compute_digest(image, source->size, record.sha256);
    if (err != ESP_OK) return err;
    uint8_t *buffer = heap_caps_malloc(BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_ERR_NO_MEM;

    /* Publish the new record only after its payload and metadata read back.
     * The previous slot remains usable across loss of power at any earlier step. */
    const size_t base = slot * SLOT_SIZE;
    const size_t blocks = (source->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    report("cache_erase", 0, blocks + 1);
    err = esp_partition_erase_range(part, base, BLOCK_SIZE);
    for (size_t i = 0; err == ESP_OK && i < blocks;) {
        size_t count = blocks - i < 16 ? blocks - i : 16;
        err = esp_partition_erase_range(part, base + (i + 1) * BLOCK_SIZE, count * BLOCK_SIZE);
        i += count;
        report("cache_erase", i + 1, blocks + 1);
        vTaskDelay(1);
    }
    for (size_t done = 0; err == ESP_OK && done < source->size;) {
        size_t length = source->size - done < BLOCK_SIZE ? source->size - done : BLOCK_SIZE;
        memcpy(buffer, image + done, length);
        err = esp_partition_write(part, base + BLOCK_SIZE + done, buffer, length);
        if (err == ESP_OK) err = esp_partition_read(part, base + BLOCK_SIZE + done, buffer, length);
        if (err == ESP_OK && memcmp(buffer, image + done, length)) err = ESP_ERR_INVALID_CRC;
        done += length;
        report("cache_write", done, source->size);
        vTaskDelay(1);
    }
    const uint32_t magic = RECORD_MAGIC;
    if (err == ESP_OK) err = esp_partition_write(part, base + sizeof(magic), &record, sizeof(record));
    if (err == ESP_OK) err = esp_partition_read(part, base + sizeof(magic), buffer, sizeof(record));
    if (err == ESP_OK && memcmp(buffer, &record, sizeof(record))) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK) err = esp_partition_write(part, base, &magic, sizeof(magic));
    free(buffer);
    return err;
}
