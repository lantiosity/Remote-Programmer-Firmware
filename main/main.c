/**
 * @file  main.c
 * @brief  ESP32-S3 多链路 DAPLink 入口
 *
 *   USB  : CMSIS-DAP v1 (HID) + CDC 虚拟串口  -> Keil / pyOCD / OpenOCD
 *   WiFi : pyOCD remote 探针协议 (TCP 5555) + 串口桥 (TCP 5556)
 *   BLE  : DAP JSON 桥 (自定义 UUID) + NUS 虚拟串口
 *
 * 接线与使用说明见 docs/。
 */
#include <inttypes.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_cfg.h"
#include "ble_dev.h"
#include "ble_ota.h"
#include "wifi_setup.h"
#include "console_cmds.h"
#include "link.h"
#include "net_dev.h"
#include "probe_core.h"
#include "serial_bridge.h"
#include "usb_dev.h"
#include "wired_ota.h"

static const char *TAG = "main";

void app_main(void)
{
    bool healthy = true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        healthy = false;
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "ESP32-S3 多链路 DAPLink 启动");

    cfg_init();
    ESP_ERROR_CHECK(wifi_setup_init());

    const cfg_probe_pins_t *pins = cfg_probe();
    probe_core_cfg_t pcfg = {
        .clk_gpio = pins->swclk_gpio,
        .dio_gpio = pins->swdio_gpio,
        .nrst_gpio = pins->nreset_gpio,
        .clock_hz = pins->clock_hz,
    };
    err = probe_init(&pcfg);
    if (err != ESP_OK) {
        healthy = false;
        ESP_LOGE(TAG, "SWD 初始化失败: %s", esp_err_to_name(err));
    }

    link_init();

    err = usb_dev_init();
    if (err != ESP_OK) {
        healthy = false;
        ESP_LOGE(TAG, "USB 初始化失败: %s", esp_err_to_name(err));
    }

    err = serial_bridge_init();
    if (err == ESP_OK) {
        serial_bridge_start();
    } else {
        healthy = false;
        ESP_LOGE(TAG, "串口桥初始化失败: %s", esp_err_to_name(err));
    }

    err = net_dev_init();
    if (err != ESP_OK) {
        healthy = false;
        ESP_LOGE(TAG, "WiFi 初始化失败: %s", esp_err_to_name(err));
    }

    err = ble_dev_init();
    if (err != ESP_OK) {
        healthy = false;
        ESP_LOGE(TAG, "BLE 初始化失败: %s", esp_err_to_name(err));
    }
    /* Keep the established USB/WiFi/BLE allocation order. If BLE is disabled,
     * initialize the independent OTA worker here for USB and WiFi. */
    if (ble_ota_init() != ESP_OK) healthy = false;

    ESP_LOGI(TAG, "Startup heap free=%lu largest=%lu", (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (wired_ota_net_start() != ESP_OK) healthy = false;
    if (console_start() != ESP_OK) healthy = false;
#ifdef CONFIG_LINK_BLE_ENABLE
    healthy = healthy && ble_dev_ready();
#endif
    ble_ota_boot_confirm(healthy);

    ESP_LOGI(TAG, "初始化完成，等待上位机连接");
    ESP_LOGI(TAG, "USB: 产品名应显示为 \"CMSIS-DAP\" 并出现一个虚拟串口");
    ESP_LOGI(TAG, "WiFi: pyocd ... --probe remote:%s:%d", net_dev_ip(), cfg_dap_tcp_port());
}
