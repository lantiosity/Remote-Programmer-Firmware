/**
 * @file  usb_dev.c
 * @brief  USB 复合设备：CMSIS-DAP v1 (HID, 64B 收发) + CDC ACM 虚拟串口。
 *
 * 描述符用 TinyUSB 的模板宏拼装，通过 esp_tinyusb 的 tinyusb_driver_install()
 * 安装（PHY 与 USB 任务由它负责）。产品字符串必须包含 "CMSIS-DAP"，
 * pyOCD/OpenOCD 都是靠这个名字识别 HID 探针的。
 */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "device/usbd_pvt.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

#include "app_cfg.h"
#include "dap_cmsis.h"
#include "link.h"
#include "serial_bridge.h"
#include "usb_dev.h"
#include "ble_ota.h"

static const char *TAG = "usb";

/* ---------------- 端点 / 接口编号 ---------------- */
#define ITF_HID        0
#define ITF_CDC        1
#define ITF_TOTAL      3

#define EP_HID_OUT     0x01
#define EP_HID_IN      0x81
#define EP_CDC_OUT     0x02
#define EP_CDC_IN      0x82
#define EP_CDC_NOTIF   0x83

#define HID_EP_SIZE    64
#define CDC_EP_SIZE    64

/* ---------------- HID 报告描述符 ----------------
 * 厂商自定义用途页 0xFF00，64 字节输入 + 64 字节输出，无 Report ID。
 * 这正是 CMSIS-DAP v1 使用的 HID 报文格式。
 */
static const uint8_t s_hid_report_desc[] = {
    0x06, 0x00, 0xFF,        /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,              /* Usage (0x01) */
    0xA1, 0x01,              /* Collection (Application) */
    0x15, 0x00,              /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,        /*   Logical Maximum (255) */
    0x75, 0x08,              /*   Report Size (8) */
    0x95, 0x40,              /*   Report Count (64) */
    0x09, 0x01,              /*   Usage (0x01) */
    0x81, 0x02,              /*   Input (Data,Var,Abs) */
    0x95, 0x40,              /*   Report Count (64) */
    0x09, 0x01,              /*   Usage (0x01) */
    0x91, 0x02,              /*   Output (Data,Var,Abs) */
    0xC0,                    /* End Collection */
};

/* ---------------- 设备描述符 ---------------- */
static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CONFIG_USB_DEVICE_VID,
    .idProduct          = CONFIG_USB_DEVICE_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),

    /* CMSIS-DAP HID 接口（输入 + 输出） */
    TUD_HID_INOUT_DESCRIPTOR(ITF_HID, 4, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_hid_report_desc),
                             EP_HID_OUT, EP_HID_IN, HID_EP_SIZE, 1),

    /* CDC ACM 虚拟串口 */
    TUD_CDC_DESCRIPTOR(ITF_CDC, 5, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, CDC_EP_SIZE),
};

/* ---------------- 字符串描述符 ---------------- */
static const char s_langid[2] = {(char)0x09, (char)0x04};
static char       s_serial_str[32];

static const char *s_string_desc[] = {
    s_langid,
    CONFIG_USB_DEVICE_MANUFACTURER,
    CONFIG_USB_DEVICE_PRODUCT,
    s_serial_str,
    "CMSIS-DAP v1 Interface",
    "ESP-SWD Virtual COM",
};

static atomic_bool s_mounted;
static atomic_uint s_usb_epoch;
typedef struct { unsigned epoch; uint8_t data[DAP_PACKET_SIZE]; } hid_packet_t;
static QueueHandle_t s_hid_tx;
static QueueHandle_t s_cdc_tx;
static void hid_tx_deferred(void *unused);
static void cdc_tx_task(void *unused);
static atomic_bool s_cdc_dtr;
static atomic_bool s_cdc_rts;

/* ---------------- TinyUSB 回调 ---------------- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    if (buffer == NULL || bufsize == 0) {
        return;
    }
    if (bufsize > DAP_PACKET_SIZE) {
        bufsize = DAP_PACKET_SIZE;
    }
    if (link_submit(LINK_SINK_USB, (int)atomic_load(&s_usb_epoch), buffer, bufsize) != ESP_OK) {
        ESP_LOGW(TAG, "HID 请求丢弃（队列满）");
    }
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        link_session_set(LINK_SINK_USB, (int)atomic_fetch_add(&s_usb_epoch, 1) + 1);
        s_mounted = true;
        usbd_defer_func(hid_tx_deferred, NULL, false);
        ESP_LOGI(TAG, "USB 已挂载：CMSIS-DAP(HID) + 虚拟串口(CDC)");
        break;
    case TINYUSB_EVENT_DETACHED:
        ble_ota_disconnected(OTA_OWNER_USB);
        s_mounted = false;
        atomic_fetch_add(&s_usb_epoch, 1);
        link_session_set(LINK_SINK_USB, -1);
        xQueueReset(s_hid_tx);
        s_cdc_dtr = false;
        s_cdc_rts = false;
        ESP_LOGI(TAG, "USB 已断开");
        break;
    default:
        break;
    }
}

static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    if (event->type == CDC_EVENT_LINE_STATE_CHANGED) {
        s_cdc_dtr = event->line_state_changed_data.dtr;
        s_cdc_rts = event->line_state_changed_data.rts;
    }
}

/* ---------------- 对外接口 ---------------- */

/* Only this deferred callback and report completion run TinyUSB HID APIs. */
static void hid_tx_deferred(void *unused)
{
    (void)unused;
    hid_packet_t packet;
    while (xQueuePeek(s_hid_tx, &packet, 0) == pdTRUE) {
        if (!s_mounted || packet.epoch != atomic_load(&s_usb_epoch)) {
            xQueueReceive(s_hid_tx, &packet, 0);
            continue;
        }
        if (!tud_hid_ready()) return;  /* Completion callback resumes the queue. */
        xQueueReceive(s_hid_tx, &packet, 0);
        if (!tud_hid_report(0, packet.data, DAP_PACKET_SIZE)) link_note_drop();
        return;
    }
}

void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t len)
{
    (void)instance; (void)report; (void)len;
    hid_tx_deferred(NULL);
}

static bool usb_hid_send_cb(int opaque, const uint8_t *data, size_t len)
{
    if (!s_hid_tx || !s_mounted || (unsigned)opaque != atomic_load(&s_usb_epoch) ||
        !data || !len || len > DAP_PACKET_SIZE) return false;
    hid_packet_t packet = {.epoch = (unsigned)opaque};
    memcpy(packet.data, data, len);
    if (xQueueSend(s_hid_tx, &packet, pdMS_TO_TICKS(100)) != pdTRUE) {
        link_note_drop();
        return false;
    }
    usbd_defer_func(hid_tx_deferred, NULL, false);
    return true;
}

esp_err_t usb_dev_init(void)
{
    s_hid_tx = xQueueCreate(8, sizeof(hid_packet_t));
    if (!s_hid_tx) return ESP_ERR_NO_MEM;
    snprintf(s_serial_str, sizeof(s_serial_str), "%s", cfg_serial_number());
    link_register_sender(LINK_SINK_USB, usb_hid_send_cb);

    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = 4096,
            .priority = 10,
            .xCoreID = 0,
        },
        .descriptor = {
            .device = &s_device_desc,
            .qualifier = NULL,
            .string = (const char **)s_string_desc,
            .string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
            .full_speed_config = s_config_desc,
            .high_speed_config = NULL,
        },
        .pm_lock_enable = false,
        .event_cb = usb_event_cb,
        .event_arg = NULL,
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install 失败: %s", esp_err_to_name(err));
        return err;
    }

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_cb,
        .callback_line_coding_changed = NULL,
    };
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CDC ACM 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    s_cdc_tx = xQueueCreate(4, sizeof(serial_output_chunk_t));
    if (!s_cdc_tx) return ESP_ERR_NO_MEM;
    if (xTaskCreate(cdc_tx_task, "cdc_tx", 3072, NULL, 7, NULL) != pdPASS) {
        vQueueDelete(s_cdc_tx);
        s_cdc_tx = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB 设备已就绪 (VID:PID = %04X:%04X)",
             (unsigned)CONFIG_USB_DEVICE_VID, (unsigned)CONFIG_USB_DEVICE_PID);
    return ESP_OK;
}

bool usb_dev_hid_ready(void)
{
    return s_mounted;
}

void usb_dev_hid_send(const uint8_t *data, size_t len)
{
    (void)usb_hid_send_cb((int)atomic_load(&s_usb_epoch), data, len);
}

bool usb_dev_cdc_connected(void)
{
    /* tud_cdc_connected() requires host DTR. A UART bridge must also serve
     * terminals that leave DTR deasserted; only require usable USB endpoints. */
    return s_mounted && tud_cdc_ready();
}

size_t usb_dev_cdc_read(uint8_t *buf, size_t len)
{
    if (!usb_dev_cdc_connected() || buf == NULL || len == 0) {
        return 0;
    }
    size_t rx = 0;
    /* 走 esp_tinyusb 的封装：内部有临界区，避免和 USB 任务抢 FIFO 指针 */
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, len, &rx) != ESP_OK) {
        return 0;
    }
    return rx;
}

void usb_dev_cdc_write(const uint8_t *data, size_t len)
{
    unsigned epoch = atomic_load(&s_usb_epoch);
    if (!s_cdc_tx || !usb_dev_cdc_connected() || !data || !len || len > SERIAL_BRIDGE_CHUNK_SIZE) return;
    serial_output_chunk_t chunk = {.token = (int)epoch, .len = len};
    memcpy(chunk.data, data, len);
    if (xQueueSend(s_cdc_tx, &chunk, 0) != pdTRUE) serial_bridge_note_drop(SERIAL_OUTPUT_USB, len);
}

static void cdc_tx_task(void *unused)
{
    (void)unused;
    serial_output_chunk_t chunk;
    for (;;) {
        if (xQueueReceive(s_cdc_tx, &chunk, portMAX_DELAY) != pdTRUE) continue;
        size_t off = 0;
        for (unsigned attempt = 0; off < chunk.len && attempt < 50; ++attempt) {
            if (!usb_dev_cdc_connected() || (unsigned)chunk.token != atomic_load(&s_usb_epoch)) break;
            off += tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, chunk.data + off, chunk.len - off);
            (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(20));
            if (off < chunk.len) vTaskDelay(1);
        }
        if (off < chunk.len) serial_bridge_note_drop(SERIAL_OUTPUT_USB, chunk.len - off);
    }
}

bool usb_dev_cdc_dtr(void)
{
    return s_cdc_dtr;
}

bool usb_dev_cdc_rts(void)
{
    return s_cdc_rts;
}
