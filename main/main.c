/*
 * BadUSB —— ESP32-S3 模拟 USB 键盘 + U盘
 *
 * payload.txt = 待打文本，按 BOOT 键开始吐字。标记语法：
 *   {ENTER} {TAB} {ESC} {SPACE} {BACKSPACE} {DEL} {INS}
 *   {UP} {DOWN} {LEFT} {RIGHT} {HOME} {END} {PGUP} {PGDN}
 *   {F1}..{F24} {CAPS} {NUM} {PRTSC} {PAUSE}
 *   {CTRL} {ALT} {SHIFT} {GUI} 修饰键；组合用 +：{CTRL+C} {CTRL+ALT+DEL} {ALT+F4} {GUI+R}
 *   {DELAY 1000} / {DELAY=1000}  停顿毫秒
 *   {{ }} 字面量花括号；\n 即回车；非 ASCII（中文）打不出来
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "driver/gpio.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "class/hid/hid_device.h"

#define BASE_PATH      "/usb"
#define PAYLOAD_PATH   "/usb/payload.txt"
#define SCRIPT_MAX     (16 * 1024)
#define HID_RID_KBD    1

static const char *TAG = "badusb";

// ---------------- USB 描述符：HID 键盘 + MSC ----------------
enum {
    ITF_HID,
    ITF_MSC,
    ITF_COUNT,
};

static const uint8_t hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_RID_KBD)),
};

#define EPNUM_HID_IN   0x81
#define EPNUM_MSC_OUT  0x02
#define EPNUM_MSC_IN   0x82

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_HID, 4, true, sizeof(hid_report_desc),
                       EPNUM_HID_IN, 16, 10),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static const char *str_desc[] = {
    (char[]){0x09, 0x04},
    "Espressif",
    "S3 BadUSB",
    "badusb-01",
    "Keyboard",
    "Flash Drive",
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
}

// ---------------- LED ----------------
static void led_set(int on)
{
#if CONFIG_BADUSB_LED_GPIO >= 0
    gpio_set_level(CONFIG_BADUSB_LED_GPIO, on);
#endif
}

static void led_blink(int times, int ms)
{
    for (int i = 0; i < times; i++) {
        led_set(1);
        vTaskDelay(pdMS_TO_TICKS(ms));
        led_set(0);
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

static void led_init(void)
{
#if CONFIG_BADUSB_LED_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BADUSB_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
#endif
}

// ---------------- 键盘发送 ----------------
static esp_err_t kbd_report(uint8_t modifier, const uint8_t keys[6])
{
    if (!tud_mounted() || !tud_hid_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    tud_hid_keyboard_report(HID_RID_KBD, modifier, keys);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_BADUSB_KEY_HOLD_MS));
    tud_hid_keyboard_report(HID_RID_KBD, 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_BADUSB_KEY_GAP_MS));
    return ESP_OK;
}

// ---------------- ASCII -> HID keycode（US 布局） ----------------
typedef struct {
    uint8_t key;
    uint8_t mod;   // KEYBOARD_MODIFIER_LEFTSHIFT 或 0
} keymap_t;

static const keymap_t k_ascii[128] = {
    [' ']  = {HID_KEY_SPACE, 0},
    ['a' ... 'z'] = {0, 0},
    ['0'] = {HID_KEY_0, 0}, ['1'] = {HID_KEY_1, 0}, ['2'] = {HID_KEY_2, 0},
    ['3'] = {HID_KEY_3, 0}, ['4'] = {HID_KEY_4, 0}, ['5'] = {HID_KEY_5, 0},
    ['6'] = {HID_KEY_6, 0}, ['7'] = {HID_KEY_7, 0}, ['8'] = {HID_KEY_8, 0},
    ['9'] = {HID_KEY_9, 0},
    ['!'] = {HID_KEY_1, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['@'] = {HID_KEY_2, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['#'] = {HID_KEY_3, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['$'] = {HID_KEY_4, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['%'] = {HID_KEY_5, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['^'] = {HID_KEY_6, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['&'] = {HID_KEY_7, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['*'] = {HID_KEY_8, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['('] = {HID_KEY_9, KEYBOARD_MODIFIER_LEFTSHIFT},
    [')'] = {HID_KEY_0, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['-'] = {HID_KEY_MINUS, 0},          ['_'] = {HID_KEY_MINUS, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['='] = {HID_KEY_EQUAL, 0},          ['+'] = {HID_KEY_EQUAL, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['['] = {HID_KEY_BRACKET_LEFT, 0},   ['{'] = {HID_KEY_BRACKET_LEFT, KEYBOARD_MODIFIER_LEFTSHIFT},
    [']'] = {HID_KEY_BRACKET_RIGHT, 0},  ['}'] = {HID_KEY_BRACKET_RIGHT, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['\\'] = {HID_KEY_BACKSLASH, 0},     ['|'] = {HID_KEY_BACKSLASH, KEYBOARD_MODIFIER_LEFTSHIFT},
    [';'] = {HID_KEY_SEMICOLON, 0},      [':'] = {HID_KEY_SEMICOLON, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['\''] = {HID_KEY_APOSTROPHE, 0},    ['"'] = {HID_KEY_APOSTROPHE, KEYBOARD_MODIFIER_LEFTSHIFT},
    [','] = {HID_KEY_COMMA, 0},          ['<'] = {HID_KEY_COMMA, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['.'] = {HID_KEY_PERIOD, 0},         ['>'] = {HID_KEY_PERIOD, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['/'] = {HID_KEY_SLASH, 0},          ['?'] = {HID_KEY_SLASH, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['`'] = {HID_KEY_GRAVE, 0},          ['~'] = {HID_KEY_GRAVE, KEYBOARD_MODIFIER_LEFTSHIFT},
    ['\n'] = {HID_KEY_ENTER, 0},         ['\t'] = {HID_KEY_TAB, 0},
};

static bool ascii_key(char c, keymap_t *out)
{
    if (c >= 'a' && c <= 'z') {
        out->key = HID_KEY_A + (c - 'a');
        out->mod = 0;
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        out->key = HID_KEY_A + (c - 'A');
        out->mod = KEYBOARD_MODIFIER_LEFTSHIFT;
        return true;
    }
    if ((unsigned char)c < 128 && k_ascii[(int)c].key) {
        *out = k_ascii[(int)c];
        return true;
    }
    return false;
}

// ---------------- 键名表 ----------------
typedef struct {
    const char *name;
    uint8_t key;     // 0 = 是修饰键
    uint8_t mod;
} keyent_t;

static const keyent_t k_keys[] = {
    {"ENTER", HID_KEY_ENTER, 0},        {"RETURN", HID_KEY_ENTER, 0},
    {"TAB", HID_KEY_TAB, 0},            {"SPACE", HID_KEY_SPACE, 0},
    {"BACKSPACE", HID_KEY_BACKSPACE, 0},{"DELETE", HID_KEY_DELETE, 0},
    {"INSERT", HID_KEY_INSERT, 0},      {"INS", HID_KEY_INSERT, 0},
    {"ESCAPE", HID_KEY_ESCAPE, 0},      {"ESC", HID_KEY_ESCAPE, 0},
    {"UP", HID_KEY_ARROW_UP, 0},        {"UPARROW", HID_KEY_ARROW_UP, 0},
    {"DOWN", HID_KEY_ARROW_DOWN, 0},    {"DOWNARROW", HID_KEY_ARROW_DOWN, 0},
    {"LEFT", HID_KEY_ARROW_LEFT, 0},    {"LEFTARROW", HID_KEY_ARROW_LEFT, 0},
    {"RIGHT", HID_KEY_ARROW_RIGHT, 0},  {"RIGHTARROW", HID_KEY_ARROW_RIGHT, 0},
    {"HOME", HID_KEY_HOME, 0},          {"END", HID_KEY_END, 0},
    {"PAGEUP", HID_KEY_PAGE_UP, 0},     {"PAGEDOWN", HID_KEY_PAGE_DOWN, 0},
    {"CAPSLOCK", HID_KEY_CAPS_LOCK, 0}, {"NUMLOCK", HID_KEY_NUM_LOCK, 0},
    {"SCROLLLOCK", HID_KEY_SCROLL_LOCK, 0},
    {"PRINTSCREEN", HID_KEY_PRINT_SCREEN, 0},
    {"PAUSE", HID_KEY_PAUSE, 0},        {"BREAK", HID_KEY_PAUSE, 0},
    {"APP", HID_KEY_APPLICATION, 0},    {"MENU", HID_KEY_APPLICATION, 0},
    {"CTRL", 0, KEYBOARD_MODIFIER_LEFTCTRL},
    {"CONTROL", 0, KEYBOARD_MODIFIER_LEFTCTRL},
    {"ALT", 0, KEYBOARD_MODIFIER_LEFTALT},
    {"SHIFT", 0, KEYBOARD_MODIFIER_LEFTSHIFT},
    {"GUI", 0, KEYBOARD_MODIFIER_LEFTGUI},
    {"WINDOWS", 0, KEYBOARD_MODIFIER_LEFTGUI},
    {"COMMAND", 0, KEYBOARD_MODIFIER_LEFTGUI},
};

static bool resolve_key(const char *tok, keymap_t *out)
{
    char up[16];
    size_t n = strlen(tok);
    if (n == 0 || n >= sizeof(up)) {
        return false;
    }
    for (size_t i = 0; i <= n; i++) {
        up[i] = toupper((unsigned char)tok[i]);
    }

    // F1..F24
    if (up[0] == 'F' && isdigit((unsigned char)up[1])) {
        int num = atoi(up + 1);
        if (num >= 1 && num <= 24) {
            out->key = HID_KEY_F1 + num - 1;
            out->mod = 0;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        if (strcmp(up, k_keys[i].name) == 0) {
            out->key = k_keys[i].key;
            out->mod = k_keys[i].mod;
            return true;
        }
    }
    // 单字符
    if (n == 1) {
        return ascii_key(tok[0], out);
    }
    return false;
}

// ---------------- 文本发射 ----------------
static char s_script[SCRIPT_MAX + 1];
static bool s_script_loaded;

static void emit_char(char c)
{
    keymap_t km;
    if (ascii_key(c, &km)) {
        uint8_t keys[6] = {km.key};
        kbd_report(km.mod, keys);
    }
}

// 处理 {} 内的标记：DELAY / 组合键（+ 连接）。返回 true 表示已处理
static bool emit_token(const char *tok, int len)
{
    if (len <= 0 || len >= 64) {
        return false;
    }
    char buf[64];
    memcpy(buf, tok, len);
    buf[len] = '\0';
    for (int i = 0; i < len; i++) {
        buf[i] = toupper((unsigned char)buf[i]);
    }

    if (len == 1 && buf[0] == '{') {
        emit_char('{');
        return true;
    }
    if (strncmp(buf, "DELAY", 5) == 0) {
        char *num = buf + 5;
        while (*num == ' ' || *num == '=' || *num == ':') {
            num++;
        }
        vTaskDelay(pdMS_TO_TICKS(atoi(num)));
        return true;
    }

    // 组合键：按 '+' 拆
    uint8_t mod = 0;
    uint8_t keys[6] = {0};
    int nkeys = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
        keymap_t km;
        if (!resolve_key(t, &km)) {
            ESP_LOGW(TAG, "无法识别的键: %s", t);
            return false;
        }
        mod |= km.mod;
        if (km.key) {
            if (nkeys >= 6) {
                return false;
            }
            keys[nkeys++] = km.key;
        }
    }
    if (!nkeys && !mod) {
        return false;
    }
    kbd_report(mod, nkeys ? keys : NULL);
    return true;
}

static void run_script(void)
{
    if (!s_script_loaded) {
        ESP_LOGW(TAG, "无脚本，跳过");
        return;
    }
    ESP_LOGI(TAG, "开始吐字");
    led_set(1);

    const char *p = s_script;
    while (*p && tud_mounted()) {
        if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (end && emit_token(p + 1, (int)(end - p - 1))) {
                p = end + 1;
                continue;
            }
            // 解析失败：'{' 当字面量打出去
        }
        emit_char(*p++);
    }

    led_set(0);
    ESP_LOGI(TAG, "吐字完毕");
}

// ---------------- 脚本加载 ----------------
static void load_payload(void)
{
    FILE *f = fopen(PAYLOAD_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "%s 不存在", PAYLOAD_PATH);
        s_script_loaded = false;
        return;
    }
    size_t n = fread(s_script, 1, SCRIPT_MAX, f);
    fclose(f);
    s_script[n] = '\0';
    s_script_loaded = true;
    ESP_LOGI(TAG, "已加载脚本 %u 字节", (unsigned)n);
}

static void ensure_payload_file(void)
{
    struct stat st;
    if (stat(PAYLOAD_PATH, &st) == 0) {
        // 旧版 DuckyScript 模板（以 REM 开头）直接换新格式
        char head[8] = {0};
        FILE *r = fopen(PAYLOAD_PATH, "r");
        bool legacy = r && fread(head, 1, 3, r) == 3 && memcmp(head, "REM", 3) == 0;
        if (r) {
            fclose(r);
        }
        if (!legacy) {
            return;
        }
    }
    FILE *f = fopen(PAYLOAD_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "创建 payload.txt 失败");
        return;
    }
    fputs("Hello! BOOT {ENTER}"
          "{DELAY 300}"
          "Ctrl+S {CTRL+S} {ENTER}"
          "{CTRL+ALT+DEL} {GUI+R} {DELAY 500}\n", f);
    fclose(f);
    ESP_LOGI(TAG, "已创建示例 payload.txt");
}

// ---------------- MSC ----------------
static void msc_cb(tinyusb_msc_storage_handle_t handle,
                   tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE &&
        event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        // 存储回到本地（host 拔出）：重新读脚本
        load_payload();
    }
}

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_storage;

static esp_err_t storage_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (!part) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(wl_mount(part, &s_wl), TAG, "wl_mount 失败");

    const tinyusb_msc_driver_config_t drv_cfg = {
        .callback = msc_cb,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&drv_cfg), TAG, "msc driver");

    const tinyusb_msc_storage_config_t cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .medium.wl_handle = s_wl,
        .fat_fs = {
            .base_path = BASE_PATH,
        },
    };
    return tinyusb_msc_new_storage_spiflash(&cfg, &s_storage);
}

// 按键触发前回读一次磁盘：host 改过文件时拿到最新内容
static void sync_payload_from_disk(void)
{
    tinyusb_msc_mount_point_t mp;
    if (tinyusb_msc_get_storage_mount_point(s_storage, &mp) != ESP_OK) {
        return;
    }
    if (mp == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        load_payload();
        return;
    }
    // 盘正挂在 host 上：临时切回本地读，读完还给 host
    if (tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_APP) == ESP_OK) {
        load_payload();
        tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_USB);
    }
}

// ---------------- 按键 ----------------
static bool boot_pressed(void)
{
#if CONFIG_BADUSB_BOOT_GPIO >= 0
    static bool last = true;
    bool now = gpio_get_level(CONFIG_BADUSB_BOOT_GPIO) != 0;
    bool pressed = last && !now;
    last = now;
    return pressed;
#else
    return false;
#endif
}

// ---------------- main ----------------
void app_main(void)
{
    led_init();
    led_set(0);

#if CONFIG_BADUSB_BOOT_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BADUSB_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
#endif

    ESP_ERROR_CHECK(storage_init());
    ensure_payload_file();
    load_payload();

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = cfg_desc;
    tusb_cfg.descriptor.string = str_desc;
    tusb_cfg.descriptor.string_count = sizeof(str_desc) / sizeof(str_desc[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    led_blink(2, 100);  // 初始化成功
    ESP_LOGI(TAG, "BadUSB 就绪：按 BOOT 键吐字");

    while (1) {
        if (tud_mounted() && boot_pressed()) {
            sync_payload_from_disk();
            run_script();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
