#include <inttypes.h>
#include <stdio.h>

#include "front_panel_view.h"

#define BG      PANEL_RGB(13, 22, 30)
#define SURFACE PANEL_RGB(25, 38, 49)
#define FOCUS   PANEL_RGB(26, 64, 67)
#define LINE    PANEL_RGB(47, 64, 76)
#define WHITE   PANEL_RGB(240, 248, 251)
#define MUTED   PANEL_RGB(166, 189, 201)
#define MINT    PANEL_RGB(87, 240, 192)
#define AMBER   PANEL_RGB(255, 194, 98)
#define RED     PANEL_RGB(255, 133, 133)

static void body(panel_scene_t *s, unsigned x, unsigned y, unsigned w, uint16_t color, const char *text)
{
    panel_text(s, x, y, w, color, PANEL_BODY, text);
}

static void title(panel_scene_t *s, const char *text, uint16_t color)
{
    panel_text(s, 14, 52, 212, color, PANEL_TITLE, text);
}

static void header(panel_scene_t *s, const panel_view_t *v)
{
    panel_rect(s, 0, 0, 240, 40, SURFACE);
    panel_rect(s, 14, 12, 4, 16, MINT);
    body(s, 26, 12, 106, WHITE, "ESP-SWD");
    body(s, 144, 12, 82, MUTED, v->version);
    panel_rect(s, 0, 39, 240, 1, LINE);
}

static void footer(panel_scene_t *s, const panel_view_t *v, const char *left, const char *right)
{
    if (*v->web_code) {
        char code[26];
        snprintf(code, sizeof(code), "WEB %s", v->web_code);
        body(s, 14, 270, 212, MINT, code);
    }
    panel_rect(s, 0, 293, 240, 27, SURFACE);
    panel_rect(s, 0, 293, 240, 1, LINE);
    body(s, 14, 300, 112, MUTED, left);
    body(s, 128, 300, 98, MINT, right);
}

static void row(panel_scene_t *s, unsigned y, unsigned h, bool selected, const char *label)
{
    panel_rect(s, 14, y, 212, h, selected ? FOCUS : SURFACE);
    if (selected) panel_rect(s, 14, y, 3, h, MINT);
    body(s, 25, y + (h - 16) / 2, 190, selected ? MINT : WHITE, label);
}

static const char *card_status(panel_card_t card)
{
    switch (card) {
    case PANEL_CARD_READY: return "存储卡已就绪";
    case PANEL_CARD_LOADING: return "正在挂载存储卡";
    case PANEL_CARD_ERROR: return "存储卡挂载失败";
    default: return "存储卡未挂载";
    }
}

static void home(panel_scene_t *s, const panel_view_t *v)
{
    title(s, "离线烧录", WHITE);
    body(s, 14, 85, 212, MUTED, "STM32H743VI");
    row(s, 112, 42, v->selected == 0, "存储卡镜像");
    row(s, 161, 42, v->selected == 1, "板载镜像");
    row(s, 210, 42, v->selected == 2, "设备信息");
    footer(s, v, "上下 选择", "确认 打开");
}

static void files(panel_scene_t *s, const panel_view_t *v)
{
    title(s, "选择镜像", WHITE);
    if (v->pages) {
        char page[20];
        snprintf(page, sizeof(page), "%u / %u%s", v->page_index + 1, v->pages, v->more ? "+" : "");
        body(s, 14, 85, 112, MUTED, "存储卡");
        body(s, 153, 85, 73, MUTED, page);
        for (unsigned i = 0; i < PANEL_FILES_VISIBLE && v->files[i]; ++i)
            row(s, 110 + 36 * i, 31, v->selected == i, v->files[i]);
        footer(s, v, "上下 / 左右", "确认 选择");
    } else {
        const char *message = v->card != PANEL_CARD_READY ? card_status(v->card) :
            v->directory_error ? "目录读取失败" : "未找到镜像";
        body(s, 14, 123, 212, v->card == PANEL_CARD_ERROR ? AMBER : WHITE, message);
        if (v->card != PANEL_CARD_LOADING)
            body(s, 14, 150, 212, MUTED, "将 .bin 放入卡的根目录");
        footer(s, v, "返回 主页", "确认 重试");
    }
}

static void file_label(panel_scene_t *s, const panel_view_t *v)
{
    body(s, 14, 85, 212, WHITE, v->filename);
    char size[36];
    snprintf(size, sizeof(size), "%zu B  %s", v->size, v->saved ? "板载" : "存储卡");
    body(s, 14, 106, 212, MUTED, size);
}

static void setup(panel_scene_t *s, const panel_view_t *v)
{
    title(s, "烧录设置", WHITE);
    file_label(s, v);
    char address[30];
    snprintf(address, sizeof(address), "地址  %08" PRIX32, v->address);
    row(s, 128, 29, v->selected == 0, address);
    row(s, 162, 29, v->selected == 1, v->reset_run ? "完成后复位运行" : "完成后保持暂停");
    row(s, 196, 29, v->selected == 2, "继续烧录");
    if (!v->saved) row(s, 230, 29, v->selected == 3, "保存到板载");
    footer(s, v, "左右 修改", "确认 继续");
}

static void meter(panel_scene_t *s, unsigned y, unsigned percent, uint16_t color)
{
    panel_rect(s, 14, y, 212, 8, LINE);
    panel_rect(s, 14, y, 212 * percent / 100, 8, color);
}

static void confirm(panel_scene_t *s, const panel_view_t *v)
{
    title(s, v->saving ? "确认保存" : "确认烧录", WHITE);
    file_label(s, v);
    if (v->saving) {
        body(s, 14, 141, 212, AMBER, "将替换当前板载镜像");
        body(s, 14, 171, 212, MUTED, "保存后无需存储卡");
    } else {
        body(s, 14, 141, 212, AMBER, "将擦除以下 Flash 范围");
        char range[28];
        snprintf(range, sizeof(range), "%08" PRIX32 " - %08" PRIX32, v->address, v->last_address);
        body(s, 14, 165, 212, WHITE, range);
        body(s, 14, 193, 212, MUTED, v->reset_run ? "成功后复位运行" : "成功后保持暂停");
    }
    body(s, 14, 232, 212, MINT, "按住确认键开始");
    meter(s, 254, v->percent, MINT);
    footer(s, v, "返回 取消", "长按 确认");
}

static void working(panel_scene_t *s, const panel_view_t *v)
{
    title(s, v->saving ? "正在保存" : "正在烧录", WHITE);
    file_label(s, v);
    body(s, 14, 147, 155, MINT, v->stage);
    char percent[8];
    snprintf(percent, sizeof(percent), "%u%%", v->percent);
    body(s, 179, 147, 47, MINT, percent);
    meter(s, 176, v->percent, MINT);
    body(s, 14, 216, 212, MUTED, v->saving ? "请保持设备供电" : "请保持目标供电");
    if (!v->saved) body(s, 14, 240, 212, MUTED, "请勿拔出存储卡");
    footer(s, v, "", "");
}

static void result(panel_scene_t *s, const panel_view_t *v)
{
    const char *heading = v->saving ? (v->success ? "保存完成" : "保存失败") :
        (v->success ? "校验通过" : "操作失败");
    const uint16_t color = v->success ? MINT : RED;
    title(s, heading, color);
    file_label(s, v);
    panel_rect(s, 14, 141, 212, 65, SURFACE);
    panel_rect(s, 14, 141, 3, 65, color);
    body(s, 24, 151, 192, WHITE, v->detail);
    body(s, 24, 176, 192, MUTED, v->stage);
    footer(s, v, "返回 主页", "确认 继续");
}

static void status(panel_scene_t *s, const panel_view_t *v)
{
    title(s, "设备信息", WHITE);
    const bool online = *v->ip != 0;
    body(s, 14, 89, 212, MUTED, online ? "局域网 IP" : "热点 IP");
    body(s, 14, 112, 212, MINT, online ? v->ip : "192.168.4.1");
    body(s, 14, 147, 212, WHITE, card_status(v->card));
    body(s, 14, 179, 212, MUTED, "按键计数");
    const char *const keys[] = {"上", "下", "左", "右", "确认", "返回"};
    for (unsigned i = 0; i < 6; ++i) {
        char count[20];
        snprintf(count, sizeof(count), "%s %u", keys[i], v->key_counts[i]);
        body(s, i % 2 ? 128 : 14, 203 + 21 * (i / 2), 96, WHITE, count);
    }
    footer(s, v, "返回 主页", v->card == PANEL_CARD_READY ? "右键 卸载" : "确认 挂载");
}

void front_panel_compose(panel_scene_t *s, const panel_view_t *v)
{
    s->background = BG;
    s->count = 0;
    header(s, v);
    switch (v->page) {
    case PANEL_HOME: home(s, v); break;
    case PANEL_FILES: files(s, v); break;
    case PANEL_SETUP: setup(s, v); break;
    case PANEL_CONFIRM: confirm(s, v); break;
    case PANEL_WORKING: working(s, v); break;
    case PANEL_RESULT: result(s, v); break;
    case PANEL_STATUS: status(s, v); break;
    }
}
