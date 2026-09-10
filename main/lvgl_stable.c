#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

extern const lv_font_t my_font;
extern const lv_font_t my_font_18;
extern const lv_font_t my_font_13;
extern const lv_img_dsc_t light_scene_image;
extern const lv_img_dsc_t dark_scene_image;
extern const lv_img_dsc_t heart_image;
extern const lv_img_dsc_t sun_image;
extern const lv_img_dsc_t moon_image;
const char *find_lunar_special_message(const struct tm *date);

/* ===== 临时测试开关：强制显示指定日期的语录，用于实机确认排版 =====
 * 设为 1 时，语录区恒定显示 TEST_FORCE_DATE 对应那条，忽略真实日期。
 * 想看某条语录的实机效果时改回 1 即可，其余代码不用动。
 * "08-10" 是所有语录里最长的一条（17 字 221px），之前用它验证过排版不溢出。 */
#define TEST_FORCE_DATE_ON  0
#define TEST_FORCE_DATE     "08-10"

/* 全包描边：以白色主本为圆心，在 (2r+1)×(2r+1) 的方格内铺满黑色副本，
 * 主本最后画、压在正中心。四边厚度一致，不是单侧阴影。
 *   r=1 → 8 黑 + 1 白，1px 边（日期 / 星期 / 结婚提示 / 时间）
 *   r=2 → 24 黑 + 1 白，2px 边（试过，48px 大字上偏粗，已退回 r=1）
 * 比单层 1px 偏移描边在浅色背景上对比度强很多，文字与图像分界清晰。 */
#define OUTLINE_MAX_RADIUS  2
#define OUTLINE_MAX_LAYERS  ((2 * OUTLINE_MAX_RADIUS + 1) * (2 * OUTLINE_MAX_RADIUS + 1))
typedef struct {
	int n;
	lv_obj_t *layers[OUTLINE_MAX_LAYERS];
} outlined_text_t;

/* 前置声明：update_scene() 早于 set_outlined_text() 定义出现，
 * 必须先告知编译器符号签名，否则会被当成非 static 隐式声明，
 * 等看到 static 定义时就报 "conflicting types"。 */
static void set_outlined_text(outlined_text_t *ot, const char *txt);
static outlined_text_t create_outlined_text(lv_obj_t *parent, const lv_font_t *font,
                                            lv_coord_t x, lv_coord_t y, lv_coord_t width,
                                            lv_text_align_t align, uint8_t radius);

static const char *TAG = "love_clock";
static lv_disp_draw_buf_t display_buffer;
static lv_disp_drv_t display_driver;
static lv_obj_t *screen;
static outlined_text_t date_text_ot;      /* 日期：18px + 8 邻居全包描边，right-align */
static outlined_text_t weekday_text_ot;   /* 星期：18px + 8 邻居全包描边，left-align */
static outlined_text_t marriage_text_ot;  /* 今天是我们结婚的：18px + 8 邻居全包描边 */
static outlined_text_t time_text_ot;  /* 时间：48px + 全包描边（厚度由 TIME_OUTLINE_R 决定） */
static lv_obj_t *days_prefix_label;
static lv_obj_t *days_label;
static lv_obj_t *days_label_bold;   /* 天数加粗副本：白色 +1,+1。按需求不加黑边 */
static lv_obj_t *days_suffix_label;
static lv_obj_t *blessing_label;
static lv_obj_t *days_prefix_shadow;
static lv_obj_t *days_suffix_shadow;
static lv_obj_t *blessing_shadow;
static lv_obj_t *sun_object;
static lv_obj_t *moon_object;
static lv_obj_t *heart_object;
static lv_obj_t *light_background;
LV_FONT_DECLARE(lv_font_montserrat_48);
static lv_obj_t *dark_background;
static i2c_master_dev_handle_t rtc_device;
static EventGroupHandle_t wifi_events;
static esp_event_handler_instance_t wifi_handler_instance;
static esp_event_handler_instance_t ip_handler_instance;
static volatile bool wifi_connected;
static time_t last_valid_time;
static spi_device_handle_t display_spi;

/* Wi-Fi 凭据：启动时从 NVS 读，读不到才用 Kconfig 编译进去的默认值。
 * 通过配网网页改过之后就一直以 NVS 里的为准，不用重新烧录。 */
#define WIFI_MAX_NETWORKS    3
#define WIFI_SSID_MAX_LEN    33   /* 32 字符 + 结束符 */
#define WIFI_PASSWORD_MAX_LEN 65  /* 64 字符 + 结束符 */
#define WIFI_NVS_NAMESPACE   "clock_wifi"

static char wifi_ssids[WIFI_MAX_NETWORKS][WIFI_SSID_MAX_LEN];
static char wifi_passwords[WIFI_MAX_NETWORKS][WIFI_PASSWORD_MAX_LEN];
static volatile bool provisioning_active;   /* 配网热点已开启，语录区改显示提示 */
static bool provisioning_hint_shown;
static long last_day_ordinal = -1;          /* 语录换日标记，配网结束后重置以恢复显示 */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1
#define RTC_ADDRESS        0x68
#define DISPLAY_WIDTH      240
#define DISPLAY_HEIGHT     320
#define DISPLAY_BUFFER_LINES 40

/* 每段刷新最多送几行。ESP32-C3 的 SPI DMA 单个描述符上限是 4092 字节，
 * 一行 240x2=480 字节，8 行 = 3840 字节留了余量。
 * 之前一次把 40 行(19200 字节)整包丢给 spi_device_transmit，由驱动自行拆
 * 描述符；一旦末尾那段没被完整送出去，丢失的正好是最后一行，屏幕上就留下
 * 一条 GRAM 残留色的横线。按行分段后每笔都在单描述符内，边界干净。 */
#define FLUSH_ROWS_PER_CHUNK 8

/* ---- 版面坐标 ----------------------------------------------------------
 * 由 tools/preview_ui.py 按 LVGL 绘制公式反解得出，改文案后重跑该脚本即可。
 *
 * 爱心图像 80x72 放在 (80,108)，但它的**墨迹**只占 x 88..151 / y 122..175，
 * 所以视觉中心是 (120, 147)，不是几何中心 (120, 144)。
 * 「第 N 天」的墨迹中心就钉在 (120, 147)。
 * ---------------------------------------------------------------------- */
#define HEART_POS_X     80
#define HEART_POS_Y     108

#define DATE_X          25    /* 日期 right-align 容器起点：文字右端 x=153（DATE_X+DATE_WIDTH）。
                               * 文字宽 126（"2026年09月19日"），实际占 x 27..153。 */
#define DATE_WIDTH      128   /* 18px「2026年09月19日」主本 126 + 8 邻居描边 +2 = 128 */
#define WEEKDAY_X       159   /* 星期 left-align 容器左端：距日期右端 6px。
                               * 文字宽 54（"星期六"），实际占 x 159..213。 */
#define WEEKDAY_WIDTH   60
/* 整体日期+星期文字范围 x 27..213，中心 x=120，屏幕正中。 */
#define ROW_DATE_Y      19    /* 日期/星期（18px）：墨迹 18..35，加描边 18..37 */
#define ROW_TIME_Y      37    /* 时间 Montserrat 48：墨迹 46..80（描边后 44..82） */
#define TIME_OUTLINE_R  1     /* 时间描边半径：1 = 上下左右各 1px 黑色（与日期同款，四边等厚） */
#define ROW_MARRIAGE_Y  95    /* 今天是我们结婚的（18px）：墨迹 94..112，加描边 94..114 */
#define ROW_DAYS_Y      135   /* 「第/数字/天」三个标签共用，于是三者基线相同、
                               * 垂直方向都落在爱心中心线上（墨迹中心 ≈147） */
#define DAYS_PREFIX_X   42    /* 「第」：容器 32，中心 58 —— 在爱心左侧外面 */
#define DAYS_PREFIX_W   32
#define DAYS_X          86    /* 天数：容器 68，中心 120 —— 水平压在爱心正中 */
#define DAYS_W          68
#define DAYS_SUFFIX_X   168   /* 「天」：容器 32，中心 184 —— 在爱心右侧外面 */
#define DAYS_SUFFIX_W   32
#define ROW_BLESSING_Y  189   /* 每日语录：墨迹 188..201 */

static const char *daily_lines[] = {
	"今天的阳光很好，你也是", "晚饭想吃什么？我请客", "风很轻，适合和你散散步",
	"下班路上，给你带了杯奶茶", "今天心情不错，因为你笑了", "天气正好，出去走走吧",
	"雨天听雷，晴天看云，身边是你", "冰箱里有你爱吃的水果", "今天换我洗碗，你歇着",
	"路边的花开了，想起你了", "别忙太晚，早点睡", "今天天气刚好，适合约会",
	"你穿那件外套特别好看", "菜买好了，回来吃饭吧", "今天有点想你，就一点点",
	"电影票订好了，走", "今天的云像一只猫，你看", "别皱眉了，走，吃顿好的",
	"你早上迷迷糊糊的样子有点可爱", "家里WiFi密码是你生日", "今天无大事，就是想和你待着",
	"这杯咖啡给你，少糖多奶", "别看了，手机没我好看", "今天穿暖点，风大",
	"超市逛一圈，给你买点零食", "今晚星星很多，出去看看？", "你做饭，我洗碗，公平吧？",
	"路上注意安全，到了说一声", "你笑起来，比今天天气还好", "别太累了，我在家等你",
	"今天也很好，明天也是"
};

typedef struct {
	const char *date;
	const char *message;
} special_message_t;

static const special_message_t special_messages[] = {
	{"01-01", "新年第一天，我们的脚步也要同步"},
	{"02-14", "今天哪里都不去，只想和你在一起"},
	{"05-20", "我知道，这是你对我的无声告白"},
	{"08-10", "从这天起，你的名字写在我家户口本上"},
	{"09-19", "那天你穿婚纱的样子，够我看一辈子"},
	{"12-25", "平安夜的雪，见证了我们的爱"},
	{"2026-02-16", "今夜陪你守岁，余生也陪你"}, {"2026-02-17", "年味有你，就是最好的味道"},
	{"2026-03-03", "灯会人多，你的手抓紧别松开"}, {"2026-06-19", "粽子飘香，我们去江边走走"},
	{"2026-08-19", "今天的星光，只为我们闪亮"}, {"2026-09-25", "月亮再圆，也不及我们团圆"},
	{"2026-10-18", "秋高气爽，一起登高看风景"},
	{"2027-02-05", "今夜陪你守岁，余生也陪你"}, {"2027-02-06", "年味有你，就是最好的味道"},
	{"2027-02-20", "灯会人多，你的手抓紧别松开"}, {"2027-06-09", "粽子飘香，我们去江边走走"},
	{"2027-08-08", "今天的星光，只为我们闪亮"}, {"2027-09-15", "月亮再圆，也不及我们团圆"},
	{"2027-10-08", "秋高气爽，一起登高看风景"},
	{"2028-01-25", "今夜陪你守岁，余生也陪你"}, {"2028-01-26", "年味有你，就是最好的味道"},
	{"2028-02-09", "灯会人多，你的手抓紧别松开"}, {"2028-05-28", "粽子飘香，我们去江边走走"},
	{"2028-08-26", "今天的星光，只为我们闪亮"}, {"2028-10-03", "月亮再圆，也不及我们团圆"},
	{"2028-10-26", "秋高气爽，一起登高看风景"},
	{"2029-02-12", "今夜陪你守岁，余生也陪你"}, {"2029-02-13", "年味有你，就是最好的味道"},
	{"2029-02-27", "灯会人多，你的手抓紧别松开"}, {"2029-06-16", "粽子飘香，我们去江边走走"},
	{"2029-08-16", "今天的星光，只为我们闪亮"}, {"2029-09-22", "月亮再圆，也不及我们团圆"},
	{"2029-10-16", "秋高气爽，一起登高看风景"},
	{"2030-02-02", "今夜陪你守岁，余生也陪你"}, {"2030-02-03", "年味有你，就是最好的味道"},
	{"2030-02-17", "灯会人多，你的手抓紧别松开"}, {"2030-06-05", "粽子飘香，我们去江边走走"},
	{"2030-08-05", "今天的星光，只为我们闪亮"}, {"2030-09-12", "月亮再圆，也不及我们团圆"},
	{"2030-10-05", "秋高气爽，一起登高看风景"},
	{"2031-01-22", "今夜陪你守岁，余生也陪你"}, {"2031-01-23", "年味有你，就是最好的味道"},
	{"2031-02-06", "灯会人多，你的手抓紧别松开"}, {"2031-06-24", "粽子飘香，我们去江边走走"},
	{"2031-08-24", "今天的星光，只为我们闪亮"}, {"2031-10-01", "月亮再圆，也不及我们团圆"},
	{"2031-10-24", "秋高气爽，一起登高看风景"},
	{"2032-02-10", "今夜陪你守岁，余生也陪你"}, {"2032-02-11", "年味有你，就是最好的味道"},
	{"2032-02-25", "灯会人多，你的手抓紧别松开"}, {"2032-06-12", "粽子飘香，我们去江边走走"},
	{"2032-08-12", "今天的星光，只为我们闪亮"}, {"2032-10-12", "秋高气爽，一起登高看风景"},
	{"2033-01-30", "今夜陪你守岁，余生也陪你"}, {"2033-01-31", "年味有你，就是最好的味道"},
	{"2033-02-14", "灯会人多，你的手抓紧别松开"}
};

static const char *find_special_message(const struct tm *date)
{
#if TEST_FORCE_DATE_ON
	/* 临时：按日期键直接查表返回，不依赖数组下标，改 TEST_FORCE_DATE 即可换句子。 */
	for (size_t i = 0; i < sizeof(special_messages) / sizeof(special_messages[0]); i++) {
		if (strcmp(special_messages[i].date, TEST_FORCE_DATE) == 0) {
			return special_messages[i].message;
		}
	}
#endif
	char full_date[16];
	char month_day[12];
	strftime(full_date, sizeof(full_date), "%Y-%m-%d", date);
	strftime(month_day, sizeof(month_day), "%m-%d", date);
	/* 优先匹配内置 special_messages[]（2026-2033 节日的原文）。 */
	for (size_t i = 0; i < sizeof(special_messages) / sizeof(special_messages[0]); i++) {
		if (strcmp(special_messages[i].date, month_day) == 0 || strcmp(special_messages[i].date, full_date) == 0) {
			return special_messages[i].message;
		}
	}
	/* 没匹配上就 fallback 到 lunar_special.c（覆盖 2026-2100，6 个节日 × 75 年）。
	 * 注：lunar_special.c 当前 450 条文案被破坏为 "???"，未修复。
	 * 修复前所有 2034 年起的节日会显示为 "???"，非节日走 daily_lines 不受影响。 */
	return find_lunar_special_message(date);
}

static uint8_t bcd_to_dec(uint8_t value)
{
	return (uint8_t)((value >> 4) * 10 + (value & 0x0f));
}

static uint8_t dec_to_bcd(uint8_t value)
{
	return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static esp_err_t rtc_read(struct tm *time_info)
{
	uint8_t reg = 0;
	uint8_t data[7] = {0};
	esp_err_t error = i2c_master_transmit_receive(rtc_device, &reg, 1, data, sizeof(data), pdMS_TO_TICKS(100));
	if (error != ESP_OK) {
		return error;
	}

	memset(time_info, 0, sizeof(*time_info));
	time_info->tm_sec = bcd_to_dec(data[0] & 0x7f);
	time_info->tm_min = bcd_to_dec(data[1] & 0x7f);
	time_info->tm_hour = bcd_to_dec(data[2] & 0x3f);
	time_info->tm_mday = bcd_to_dec(data[4] & 0x3f);
	time_info->tm_mon = bcd_to_dec(data[5] & 0x1f) - 1;
	time_info->tm_year = bcd_to_dec(data[6]) + 100;

	if (time_info->tm_year < 100 || time_info->tm_mday < 1 || time_info->tm_mon < 0 || time_info->tm_mon > 11) {
		return ESP_ERR_INVALID_STATE;
	}
	return ESP_OK;
}

static esp_err_t rtc_write(const struct tm *time_info)
{
	uint8_t data[8] = {
		0,
		dec_to_bcd((uint8_t)time_info->tm_sec),
		dec_to_bcd((uint8_t)time_info->tm_min),
		dec_to_bcd((uint8_t)time_info->tm_hour),
		dec_to_bcd((uint8_t)(time_info->tm_wday == 0 ? 7 : time_info->tm_wday)),
		dec_to_bcd((uint8_t)time_info->tm_mday),
		dec_to_bcd((uint8_t)(time_info->tm_mon + 1)),
		dec_to_bcd((uint8_t)(time_info->tm_year - 100))
	};
	return i2c_master_transmit(rtc_device, data, sizeof(data), pdMS_TO_TICKS(100));
}

static void display_write_command(uint8_t command)
{
	gpio_set_level(CONFIG_APP_ILI9341_DC_GPIO, 0);
	spi_transaction_t transaction = {
		.length = 8,
		.tx_data = {command},
		.flags = SPI_TRANS_USE_TXDATA,
	};
	ESP_ERROR_CHECK(spi_device_transmit(display_spi, &transaction));
}

static void display_write_data(const uint8_t *data, size_t length)
{
	gpio_set_level(CONFIG_APP_ILI9341_DC_GPIO, 1);
	spi_transaction_t transaction = {
		.length = length * 8,
		.tx_buffer = data,
	};
	ESP_ERROR_CHECK(spi_device_transmit(display_spi, &transaction));
}

static void display_init_ili9341(void)
{
	gpio_config_t control = {
		.pin_bit_mask = (1ULL << CONFIG_APP_ILI9341_DC_GPIO) | (1ULL << CONFIG_APP_ILI9341_RST_GPIO),
		.mode = GPIO_MODE_OUTPUT,
	};
	ESP_ERROR_CHECK(gpio_config(&control));
	gpio_set_level(CONFIG_APP_ILI9341_RST_GPIO, 0);
	vTaskDelay(pdMS_TO_TICKS(20));
	gpio_set_level(CONFIG_APP_ILI9341_RST_GPIO, 1);
	vTaskDelay(pdMS_TO_TICKS(120));

	const uint8_t power_control_1[] = {0x23};
	const uint8_t power_control_2[] = {0x10};
	const uint8_t vcom_control_1[] = {0x2B, 0x2B};
	const uint8_t vcom_control_2[] = {0xC0};
	const uint8_t memory_access[] = {0x48};
	const uint8_t pixel_format[] = {0x55};
	const uint8_t frame_rate[] = {0x00, 0x18};
	const uint8_t display_function[] = {0x08, 0x82, 0x27};
	const uint8_t gamma_positive[] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00};
	const uint8_t gamma_negative[] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F};
	const uint8_t column_end[] = {0x00, 0x00, 0x00, 0xEF};
	const uint8_t page_end[] = {0x00, 0x00, 0x01, 0x3F};

	display_write_command(0x01);
	vTaskDelay(pdMS_TO_TICKS(5));
	display_write_command(0xCB); display_write_data((const uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5);
	display_write_command(0xCF); display_write_data((const uint8_t[]){0x00, 0xC1, 0x30}, 3);
	display_write_command(0xE8); display_write_data((const uint8_t[]){0x85, 0x00, 0x78}, 3);
	display_write_command(0xEA); display_write_data((const uint8_t[]){0x00, 0x00}, 2);
	display_write_command(0xED); display_write_data((const uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4);
	display_write_command(0xF7); display_write_data((const uint8_t[]){0x20}, 1);
	display_write_command(0xF2); display_write_data((const uint8_t[]){0x00}, 1);
	display_write_command(0xC0); display_write_data(power_control_1, 1);
	display_write_command(0xC1); display_write_data(power_control_2, 1);
	display_write_command(0xC5); display_write_data(vcom_control_1, 2);
	display_write_command(0xC7); display_write_data(vcom_control_2, 1);
	display_write_command(0x36); display_write_data(memory_access, 1);
	display_write_command(0x3A); display_write_data(pixel_format, 1);
	display_write_command(0xB1); display_write_data(frame_rate, 2);
	display_write_command(0xB6); display_write_data(display_function, 3);
	display_write_command(0xE0); display_write_data(gamma_positive, sizeof(gamma_positive));
	display_write_command(0xE1); display_write_data(gamma_negative, sizeof(gamma_negative));
	display_write_command(0x2A); display_write_data(column_end, sizeof(column_end));
	display_write_command(0x2B); display_write_data(page_end, sizeof(page_end));
	display_write_command(0x11);
	vTaskDelay(pdMS_TO_TICKS(120));
	display_write_command(0x29);
	vTaskDelay(pdMS_TO_TICKS(20));
}

/* 设置 ILI9341 的列/页地址窗口（0x2A / 0x2B） */
static void display_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
	uint8_t column_data[] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
	uint8_t page_data[] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
	display_write_command(0x2A); display_write_data(column_data, sizeof(column_data));
	display_write_command(0x2B); display_write_data(page_data, sizeof(page_data));
}

/* 把一整块像素按「整行」切成 ≤ FLUSH_ROWS_PER_CHUNK 行的小段送出。
 * 0x2C 之后 ILI9341 处于连续内存写模式，窗口内地址自动递增，
 * 所以拆成多笔 SPI 传输是安全的，画面不会错位。 */
static void display_send_pixels(const lv_color_t *color_map, uint16_t width, uint16_t rows)
{
	const uint32_t row_bytes = (uint32_t)width * sizeof(lv_color_t);
	const uint8_t *cursor = (const uint8_t *)color_map;

	while (rows > 0) {
		uint16_t chunk = rows > FLUSH_ROWS_PER_CHUNK ? FLUSH_ROWS_PER_CHUNK : rows;
		display_write_data(cursor, row_bytes * chunk);
		cursor += row_bytes * chunk;
		rows -= chunk;
	}
}

static void display_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *color_map)
{
	/* 防御：LVGL 理论上不会给出越界区域，钳一下避免把窗口设到 GRAM 之外 */
	uint16_t x1 = (uint16_t)LV_MAX(area->x1, 0);
	uint16_t y1 = (uint16_t)LV_MAX(area->y1, 0);
	uint16_t x2 = (uint16_t)LV_MIN(area->x2, DISPLAY_WIDTH - 1);
	uint16_t y2 = (uint16_t)LV_MIN(area->y2, DISPLAY_HEIGHT - 1);
	if (x2 < x1 || y2 < y1) {
		lv_disp_flush_ready(driver);
		return;
	}

	display_set_window(x1, y1, x2, y2);
	display_write_command(0x2C);
	display_send_pixels(color_map, x2 - x1 + 1, y2 - y1 + 1);
	lv_disp_flush_ready(driver);
}

static void lvgl_tick(void *arg)
{
	(void)arg;
	lv_tick_inc(2);
}

static void initialize_display(void)
{
	spi_bus_config_t bus_config = {
		.sclk_io_num = CONFIG_APP_ILI9341_SCLK_GPIO,
		.mosi_io_num = CONFIG_APP_ILI9341_MOSI_GPIO,
		.miso_io_num = -1,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_BUFFER_LINES * sizeof(lv_color_t),
	};
	ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_APP_ILI9341_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

	spi_device_interface_config_t device_config = {
		.clock_speed_hz = CONFIG_APP_ILI9341_SPI_HZ,
		.mode = 0,
		.spics_io_num = CONFIG_APP_ILI9341_CS_GPIO,
		.queue_size = 7,
	};
	ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_APP_ILI9341_SPI_HOST, &device_config, &display_spi));
	display_init_ili9341();

	gpio_config_t backlight = {
		.pin_bit_mask = 1ULL << CONFIG_APP_ILI9341_BACKLIGHT_GPIO,
		.mode = GPIO_MODE_OUTPUT,
	};
	ESP_ERROR_CHECK(gpio_config(&backlight));
	ESP_ERROR_CHECK(gpio_set_level(CONFIG_APP_ILI9341_BACKLIGHT_GPIO, 1));

	lv_init();
	lv_color_t *buffer_1 = heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_BUFFER_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA);
	lv_color_t *buffer_2 = heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_BUFFER_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA);
	assert(buffer_1 != NULL && buffer_2 != NULL);
	lv_disp_draw_buf_init(&display_buffer, buffer_1, buffer_2, DISPLAY_WIDTH * DISPLAY_BUFFER_LINES);
	lv_disp_drv_init(&display_driver);
	display_driver.hor_res = DISPLAY_WIDTH;
	display_driver.ver_res = DISPLAY_HEIGHT;
	display_driver.flush_cb = display_flush;
	display_driver.draw_buf = &display_buffer;
	lv_disp_drv_register(&display_driver);

	const esp_timer_create_args_t tick_args = {.callback = lvgl_tick, .name = "lvgl_tick"};
	esp_timer_handle_t tick_timer;
	ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
	ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000));
}

static void initialize_rtc(void)
{
	i2c_master_bus_config_t bus_config = {
		.i2c_port = I2C_NUM_0,
		.sda_io_num = CONFIG_APP_DS3231_SDA_GPIO,
		.scl_io_num = CONFIG_APP_DS3231_SCL_GPIO,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	i2c_master_bus_handle_t bus_handle;
	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));
	i2c_device_config_t device_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = RTC_ADDRESS,
		.scl_speed_hz = CONFIG_APP_DS3231_I2C_HZ,
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &device_config, &rtc_device));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	(void)arg;
	(void)event_data;
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_connected = false;
		xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		wifi_connected = true;
		xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
	}
}

/* 从 NVS 读取 Wi-Fi 凭据；某个键不存在就用 Kconfig 的默认值补上。
 * 这样第一次烧录（NVS 空）时行为跟以前完全一致，改过之后则以 NVS 为准。 */
static void load_wifi_credentials(void)
{
	const char *default_ssids[] = {CONFIG_APP_WIFI_SSID_1, CONFIG_APP_WIFI_SSID_2, CONFIG_APP_WIFI_SSID_3};
	const char *default_passwords[] = {CONFIG_APP_WIFI_PASSWORD_1, CONFIG_APP_WIFI_PASSWORD_2, CONFIG_APP_WIFI_PASSWORD_3};

	for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
		strlcpy(wifi_ssids[i], default_ssids[i], WIFI_SSID_MAX_LEN);
		strlcpy(wifi_passwords[i], default_passwords[i], WIFI_PASSWORD_MAX_LEN);
	}

	nvs_handle_t handle;
	if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
		ESP_LOGI(TAG, "no stored Wi-Fi config, using compiled defaults");
		return;
	}
	char key[8];
	for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
		size_t length = WIFI_SSID_MAX_LEN;
		snprintf(key, sizeof(key), "ssid%d", i);
		if (nvs_get_str(handle, key, wifi_ssids[i], &length) != ESP_OK) {
			strlcpy(wifi_ssids[i], default_ssids[i], WIFI_SSID_MAX_LEN);
		}
		length = WIFI_PASSWORD_MAX_LEN;
		snprintf(key, sizeof(key), "pass%d", i);
		if (nvs_get_str(handle, key, wifi_passwords[i], &length) != ESP_OK) {
			strlcpy(wifi_passwords[i], default_passwords[i], WIFI_PASSWORD_MAX_LEN);
		}
	}
	nvs_close(handle);
	ESP_LOGI(TAG, "loaded Wi-Fi config from NVS");
}

/* 把当前这组凭据写进 NVS */
static esp_err_t save_wifi_credentials(void)
{
	nvs_handle_t handle;
	esp_err_t error = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (error != ESP_OK) {
		return error;
	}
	char key[8];
	for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
		snprintf(key, sizeof(key), "ssid%d", i);
		nvs_set_str(handle, key, wifi_ssids[i]);
		snprintf(key, sizeof(key), "pass%d", i);
		nvs_set_str(handle, key, wifi_passwords[i]);
	}
	error = nvs_commit(handle);
	nvs_close(handle);
	return error;
}

/* 配网任务也会周期性重试已保存的 Wi-Fi，和定时校时任务可能撞车，
 * 所以整体串行化：同一时刻只允许一个任务在做 STA 连接。 */
static SemaphoreHandle_t wifi_connect_lock;

static bool try_wifi_network(const char *ssid, const char *password)
{
	if (ssid[0] == '\0') {
		return false;
	}
	if (wifi_connect_lock != NULL) {
		xSemaphoreTake(wifi_connect_lock, portMAX_DELAY);
	}
	wifi_config_t config = {0};
	strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
	strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
	xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
	ESP_ERROR_CHECK(esp_wifi_connect());
	EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(12000));
	if (!(bits & WIFI_CONNECTED_BIT)) {
		esp_wifi_disconnect();
	}
	if (wifi_connect_lock != NULL) {
		xSemaphoreGive(wifi_connect_lock);
	}
	return (bits & WIFI_CONNECTED_BIT) != 0;
}

static bool connect_wifi(void)
{
	wifi_events = xEventGroupCreate();
	wifi_connect_lock = xSemaphoreCreateMutex();
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();
	wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init_config));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &wifi_handler_instance));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &ip_handler_instance));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());

	const char *labels[] = {"Wi-Fi 1", "Wi-Fi 2", "Wi-Fi 3"};
	for (int i = 0; i < CONFIG_APP_WIFI_NETWORK_COUNT; i++) {
		ESP_LOGI(TAG, "trying %s: %s", labels[i], wifi_ssids[i]);
		if (try_wifi_network(wifi_ssids[i], wifi_passwords[i])) {
			ESP_LOGI(TAG, "Wi-Fi connected");
			return true;
		}
	}
	ESP_LOGW(TAG, "no configured Wi-Fi is available");
	return false;
}

static void synchronize_time(void)
{
	setenv("TZ", "CST-8", 1);
	tzset();
	struct tm rtc_time;
	bool rtc_valid = rtc_read(&rtc_time) == ESP_OK;
	if (rtc_valid) {
		last_valid_time = mktime(&rtc_time);
		struct timeval now = {.tv_sec = last_valid_time};
		settimeofday(&now, NULL);
	}

	if (wifi_connected) {
		esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
		esp_sntp_setservername(0, "pool.ntp.org");
		esp_sntp_init();
		time_t before = time(NULL);
		for (int attempt = 0; attempt < 20; attempt++) {
			vTaskDelay(pdMS_TO_TICKS(500));
			time_t current = time(NULL);
			if (current > before + 60) {
				struct tm network_time;
				localtime_r(&current, &network_time);
				rtc_write(&network_time);
				last_valid_time = current;
				ESP_LOGI(TAG, "time synchronized by NTP");
				return;
			}
		}
		esp_sntp_stop();
	}
	if (!rtc_valid) {
		ESP_LOGW(TAG, "RTC and NTP are both unavailable");
	}
}

/* ---- 定时 NTP 校准 ----------------------------------------------------
 * 除了上电那一次，每天 0 点再校准一次。
 * 整套流程跑在独立任务里：连 Wi-Fi 最多卡 12 秒、等 NTP 又十几秒，
 * 放进主循环会把 UI 卡住（这屏之前就是因为脏区重绘卡过）。
 * 拿不到网络就回退读 DS3231，把系统时间拉回来。 */
#define NTP_SYNC_HOUR        0       /* 每天校准的整点（0 = 午夜） */
#define NTP_SYNC_CHECK_MS    30000   /* 每 30 秒看一次是否到点 */
#define NTP_SYNC_TIMEOUT_MS  15000   /* 单轮 NTP 最多等 15 秒 */

static volatile bool ntp_sync_finished;

static void ntp_sync_callback(struct timeval *tv)
{
	(void)tv;
	ntp_sync_finished = true;
}

/* 用 DS3231 的时间兜底校准系统时间 */
static bool sync_time_from_rtc(void)
{
	struct tm rtc_time;
	if (rtc_read(&rtc_time) != ESP_OK) {
		return false;
	}
	time_t rtc_seconds = mktime(&rtc_time);
	struct timeval now = {.tv_sec = rtc_seconds};
	settimeofday(&now, NULL);
	last_valid_time = rtc_seconds;
	ESP_LOGI(TAG, "time restored from RTC");
	return true;
}

/* 跑一轮 SNTP，成功后把时间回写进 DS3231 */
static bool sync_time_from_ntp(uint32_t timeout_ms)
{
	ntp_sync_finished = false;
	sntp_set_time_sync_notification_cb(ntp_sync_callback);
	esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
	esp_sntp_setservername(0, "pool.ntp.org");
	/* 上电那次 NTP 成功后 SNTP 是留在运行状态的（默认还会每小时自动同步），
	 * 这里先停掉，保证每轮定时校准都从干净状态开始。 */
	esp_sntp_stop();
	esp_sntp_init();

	for (uint32_t waited = 0; waited < timeout_ms && !ntp_sync_finished; waited += 500) {
		vTaskDelay(pdMS_TO_TICKS(500));
	}
	bool synced = ntp_sync_finished;
	if (synced) {
		time_t now = time(NULL);
		struct tm network_time;
		localtime_r(&now, &network_time);
		rtc_write(&network_time);
		last_valid_time = now;
		ESP_LOGI(TAG, "time synchronized by NTP");
	}
	esp_sntp_stop();
	return synced;
}

/* 断线后重连已配置的 Wi-Fi。connect_wifi() 里的 netif/wifi 初始化只能跑一次，
 * 所以这里只重试连接，不重复初始化。 */
static bool ensure_wifi_connection(void)
{
	if (wifi_connected) {
		return true;
	}
	for (int i = 0; i < CONFIG_APP_WIFI_NETWORK_COUNT; i++) {
		if (try_wifi_network(wifi_ssids[i], wifi_passwords[i])) {
			wifi_connected = true;
			ESP_LOGI(TAG, "Wi-Fi reconnected to %s", wifi_ssids[i]);
			return true;
		}
	}
	return false;
}

static void time_sync_task(void *arg)
{
	(void)arg;
	int last_marker = -1;

	while (true) {
		vTaskDelay(pdMS_TO_TICKS(NTP_SYNC_CHECK_MS));

		time_t now = time(NULL);
		if (now < 100000) {                 /* 系统时间还没建立起来 */
			continue;
		}
		struct tm now_tm;
		localtime_r(&now, &now_tm);

		if (now_tm.tm_hour != NTP_SYNC_HOUR) {
			continue;
		}
		/* 用「第几天 * 24 + 小时」做标记，同一天只校准一次 */
		int marker = now_tm.tm_yday * 24 + now_tm.tm_hour;
		if (marker == last_marker) {
			continue;
		}
		last_marker = marker;

		ESP_LOGI(TAG, "scheduled time sync at %02d:%02d", now_tm.tm_hour, now_tm.tm_min);
		if (ensure_wifi_connection() && sync_time_from_ntp(NTP_SYNC_TIMEOUT_MS)) {
			continue;
		}
		sync_time_from_rtc();
	}
}

/* ---- SoftAP 网页配网 ---------------------------------------------------
 * 所有已配置的 Wi-Fi 都连不上时，开一个开放热点，手机/电脑连上后浏览器打开
 * 192.168.4.1 填表，存进 NVS 后自动重启。以后增减 Wi-Fi 就不用重新烧录了。
 * 只在连不上网的时候才启动，平时不占内存、不开热点。 */
#define PROV_AP_SSID       "ClockSetup"
#define PROV_AP_CHANNEL    1
#define PROV_AP_MAX_CONN   2
#define PROV_HTTP_PORT     80
/* 热点不会一直开着：没人连进来就 PROV_AP_IDLE_TIMEOUT_MS 后自动关掉，
 * 屏幕恢复正常语录。这样长期断网也只是个普通时钟，不会常驻一个开放热点。
 * 有设备连进来配网时计时会不断重置，不用担心填表填到一半被关掉。
 * 需要重新配网时，断电重启一次就会再开一轮。 */
#define PROV_AP_IDLE_TIMEOUT_MS  (10 * 60 * 1000)
/* 兜底：手机连着但不填表也不能无限挂着 */
#define PROV_AP_MAX_LIFETIME_MS  (30 * 60 * 1000)
#define PROV_POLL_INTERVAL_MS    5000    /* 检查空闲/重连的间隔 */
#define PROV_WIFI_RETRY_MS       60000   /* 断网期间每隔多久重试一次已保存的 Wi-Fi */
#define PROV_FORM_MAX_LEN  512

static const char *const PROV_HTML =
	"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>时钟配网</title><style>"
	"body{font-family:sans-serif;max-width:22rem;margin:2rem auto;padding:0 1rem;line-height:1.6}"
	"h2{font-size:1.1rem}label{font-size:.85rem;color:#555}"
	/* 这串 HTML 是 snprintf 的格式串，CSS 里字面的 % 必须写成 %%，
	 * 否则 %. 会被当成非法转换符（编译期 -Werror=format 直接报错） */
	"input{width:100%%;padding:.5rem;margin:.15rem 0 .8rem;border:1px solid #ccc;border-radius:4px;box-sizing:border-box}"
	"button{width:100%%;padding:.7rem;background:#e86a80;color:#fff;border:0;border-radius:4px;font-size:1rem}"
	"</style></head><body><h2>结婚纪念日时钟 · 配网</h2>"
	"<p style=\"font-size:.85rem;color:#666\">填好后保存，设备会自动重启连接。用不到的行留空即可。</p>"
	"<form method=\"post\" action=\"/save\">"
	"<label>Wi-Fi 1</label><input name=\"ssid0\" placeholder=\"名称\" value=\"%s\"><input name=\"pass0\" placeholder=\"密码\" value=\"%s\">"
	"<label>Wi-Fi 2</label><input name=\"ssid1\" placeholder=\"名称\" value=\"%s\"><input name=\"pass1\" placeholder=\"密码\" value=\"%s\">"
	"<label>Wi-Fi 3</label><input name=\"ssid2\" placeholder=\"名称\" value=\"%s\"><input name=\"pass2\" placeholder=\"密码\" value=\"%s\">"
	"<button type=\"submit\">保存并重启</button></form></body></html>";

/* 表单是 x-www-form-urlencoded，中文 SSID 会被百分号编码，这里还原回原始字节 */
static void url_decode(char *destination, size_t destination_size, const char *source, size_t source_length)
{
	size_t written = 0;
	for (size_t i = 0; i < source_length && written + 1 < destination_size; i++) {
		if (source[i] == '%' && i + 2 < source_length) {
			char hex[3] = {source[i + 1], source[i + 2], '\0'};
			destination[written++] = (char)strtol(hex, NULL, 16);
			i += 2;
		} else if (source[i] == '+') {
			destination[written++] = ' ';
		} else {
			destination[written++] = source[i];
		}
	}
	destination[written] = '\0';
}

static void parse_form_field(const char *body, const char *name, char *output, size_t output_size)
{
	char pattern[12];
	snprintf(pattern, sizeof(pattern), "%s=", name);
	const char *start = strstr(body, pattern);
	if (start == NULL) {
		return;
	}
	start += strlen(pattern);
	const char *end = strchr(start, '&');
	url_decode(output, output_size, start, end != NULL ? (size_t)(end - start) : strlen(start));
}

static esp_err_t provisioning_get_handler(httpd_req_t *request)
{
	/* 页面渲染 buffer 放静态区：httpd 任务栈只有几 KB，2300 字节压在栈上再加
	 * snprintf 的开销会直接溢出崩溃（表现为手机一连热点设备就重启）。
	 * httpd 只有一个任务串行调 handler，静态 buffer 不会有并发问题。 */
	static char page[2300];
	int length = snprintf(page, sizeof(page), PROV_HTML,
			wifi_ssids[0], wifi_passwords[0],
			wifi_ssids[1], wifi_passwords[1],
			wifi_ssids[2], wifi_passwords[2]);
	if (length < 0 || length >= (int)sizeof(page)) {
		return ESP_FAIL;
	}
	return httpd_resp_send(request, page, length);
}

static void provisioning_restart_task(void *arg)
{
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(1500));   /* 让浏览器先把响应收完 */
	esp_restart();
}

static esp_err_t provisioning_post_handler(httpd_req_t *request)
{
	char body[PROV_FORM_MAX_LEN];
	int total = 0;
	size_t remaining = request->content_len;

	while (remaining > 0 && (size_t)total < sizeof(body) - 1) {
		size_t space = sizeof(body) - 1 - (size_t)total;
		int received = httpd_req_recv(request, body + total, remaining < space ? remaining : space);
		if (received <= 0) {
			break;
		}
		total += received;
		remaining -= (size_t)received;
	}
	body[total] = '\0';

	for (int i = 0; i < WIFI_MAX_NETWORKS; i++) {
		char name[8];
		snprintf(name, sizeof(name), "ssid%d", i);
		parse_form_field(body, name, wifi_ssids[i], WIFI_SSID_MAX_LEN);
		snprintf(name, sizeof(name), "pass%d", i);
		parse_form_field(body, name, wifi_passwords[i], WIFI_PASSWORD_MAX_LEN);
		ESP_LOGI(TAG, "provisioned Wi-Fi %d: %s", i + 1, wifi_ssids[i]);
	}

	esp_err_t error = save_wifi_credentials();
	const char *response = error == ESP_OK
			? "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body style=\"font-family:sans-serif\">"
			  "<h2>已保存，正在重启…</h2></body></html>"
			: "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body style=\"font-family:sans-serif\">"
			  "<h2>保存失败，请重试</h2></body></html>";
	httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);

	if (error == ESP_OK) {
		xTaskCreate(provisioning_restart_task, "prov_restart", 2048, NULL, 1, NULL);
	}
	return ESP_OK;
}

/* 当前有没有设备连在 ClockSetup 热点上（正在配网）。 */
static bool provisioning_has_client(void)
{
	wifi_sta_list_t stations;
	if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) {
		return false;
	}
	return stations.num > 0;
}

static void provisioning_task(void *arg)
{
	(void)arg;
	ESP_LOGW(TAG, "no known Wi-Fi reachable, starting setup portal '%s'", PROV_AP_SSID);

	esp_netif_create_default_wifi_ap();
	esp_err_t error = esp_wifi_set_mode(WIFI_MODE_APSTA);
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "failed to switch to APSTA: %s", esp_err_to_name(error));
		vTaskDelete(NULL);
		return;
	}

	wifi_config_t ap_config = {0};
	strlcpy((char *)ap_config.ap.ssid, PROV_AP_SSID, sizeof(ap_config.ap.ssid));
	ap_config.ap.ssid_len = (uint8_t)strlen(PROV_AP_SSID);
	ap_config.ap.channel = PROV_AP_CHANNEL;
	ap_config.ap.authmode = WIFI_AUTH_OPEN;      /* 开放热点，配网用，不设密码 */
	ap_config.ap.max_connection = PROV_AP_MAX_CONN;
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = PROV_HTTP_PORT;
	config.stack_size = 8192;      /* 默认 4096 太小，处理页面请求时容易栈溢出 */
	config.max_open_sockets = 4;   /* 手机连上会并发发探测请求，收一点省内存 */
	httpd_handle_t server = NULL;
	if (httpd_start(&server, &config) != ESP_OK) {
		ESP_LOGE(TAG, "failed to start http server");
		vTaskDelete(NULL);
		return;
	}
	httpd_uri_t get_uri = {
		.uri = "/", .method = HTTP_GET, .handler = provisioning_get_handler, .user_ctx = NULL,
	};
	httpd_uri_t post_uri = {
		.uri = "/save", .method = HTTP_POST, .handler = provisioning_post_handler, .user_ctx = NULL,
	};
	httpd_register_uri_handler(server, &get_uri);
	httpd_register_uri_handler(server, &post_uri);

	provisioning_active = true;
	ESP_LOGI(TAG, "setup portal ready: join '%s' then open http://192.168.4.1", PROV_AP_SSID);

	/* 常驻看护：没人连就超时关热点，网络自己恢复了就悄悄接回去。
	 * 长期断网时设备退回成一块普通时钟（时间靠 DS3231 走），
	 * 不会一直挂着一个开放热点，也不会一直占着语录区。 */
	uint32_t idle_ms = 0;
	uint32_t alive_ms = 0;
	uint32_t since_retry_ms = 0;
	bool wifi_recovered = false;

	while (true) {
		vTaskDelay(pdMS_TO_TICKS(PROV_POLL_INTERVAL_MS));
		idle_ms += PROV_POLL_INTERVAL_MS;
		alive_ms += PROV_POLL_INTERVAL_MS;
		since_retry_ms += PROV_POLL_INTERVAL_MS;

		if (alive_ms >= PROV_AP_MAX_LIFETIME_MS) {
			ESP_LOGW(TAG, "setup portal lifetime reached, closing AP");
			break;
		}
		if (provisioning_has_client()) {
			idle_ms = 0;            /* 有人正在配网，别关，也别去抢 Wi-Fi */
			continue;
		}
		if (since_retry_ms >= PROV_WIFI_RETRY_MS) {
			since_retry_ms = 0;
			if (ensure_wifi_connection()) {
				ESP_LOGI(TAG, "Wi-Fi back, closing setup portal");
				wifi_recovered = true;
				break;
			}
		}
		if (idle_ms >= PROV_AP_IDLE_TIMEOUT_MS) {
			ESP_LOGW(TAG, "setup portal idle for %d min, closing AP", PROV_AP_IDLE_TIMEOUT_MS / 60000);
			break;
		}
	}

	httpd_stop(server);
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));

	if (wifi_recovered) {
		/* 切模式会重启 Wi-Fi 接口，重连一次并顺手校个时 */
		wifi_connected = false;
		if (ensure_wifi_connection()) {
			sync_time_from_ntp(NTP_SYNC_TIMEOUT_MS);
		}
	}

	/* 交还语录区：只改标志位，文本由 update_scene 在 LVGL 线程里刷 */
	provisioning_active = false;
	provisioning_hint_shown = false;
	last_day_ordinal = -1;
	vTaskDelete(NULL);
}

static int calculate_marriage_days(const struct tm *now)
{
	struct tm anniversary = *now;
	anniversary.tm_year = CONFIG_APP_MARRIAGE_YEAR - 1900;
	anniversary.tm_mon = CONFIG_APP_MARRIAGE_MONTH - 1;
	anniversary.tm_mday = CONFIG_APP_MARRIAGE_DAY;
	anniversary.tm_hour = 0;
	anniversary.tm_min = 0;
	anniversary.tm_sec = 0;
	time_t anniversary_time = mktime(&anniversary);
	time_t now_time = mktime((struct tm *)now);
	if (now_time < anniversary_time) {
		return 0;
	}
	return (int)((now_time - anniversary_time) / 86400) + 1;
}

static void update_scene(lv_timer_t *timer)
{
	(void)timer;
	time_t now = time(NULL);
	struct tm local_time;
	localtime_r(&now, &local_time);
	const char *weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
	char date_text[64];
	char time_text[16];
	char days_text[32];
	snprintf(date_text, sizeof(date_text), "%d年%02d月%02d日", local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday);
	snprintf(time_text, sizeof(time_text), "%02d:%02d", local_time.tm_hour, local_time.tm_min);
	snprintf(days_text, sizeof(days_text), "%d", calculate_marriage_days(&local_time));
	set_outlined_text(&date_text_ot, date_text);
	set_outlined_text(&weekday_text_ot, weekdays[local_time.tm_wday]);
	/* 时间：全包描边（r=TIME_OUTLINE_R），黑副本在外、白主本压中心。
	 * 只在分钟跳变时才写文本，避免每秒触发十几层 48px 大字重绘。 */
	static char last_time_text[16] = "";
	if (strcmp(time_text, last_time_text) != 0) {
		strcpy(last_time_text, time_text);
		set_outlined_text(&time_text_ot, time_text);
	}
	/* 天数：白字 + 白色加粗副本，无黑边 */
	lv_label_set_text(days_label, days_text);
	lv_label_set_text(days_label_bold, days_text);

	/* 语录：每天本地 0 点换一条，同一天内保持不变（重启后仍是同一条）。
	 * day_ordinal = 年 * 366 + 年内第几天，每跨一天自增 1，且不含时区运算。
	 * 配网模式下整块让位给热点提示（LVGL 不是线程安全的，所以文本只在
	 * 这里改，provisioning_task 只负责置标志位）。 */
	long day_ordinal = (long)local_time.tm_year * 366 + local_time.tm_yday;
	if (provisioning_active) {
		if (!provisioning_hint_shown) {
			provisioning_hint_shown = true;
			lv_label_set_text(blessing_shadow, "请连接热点 ClockSetup");
			lv_label_set_text(blessing_label, "请连接热点 ClockSetup");
		}
	} else if (day_ordinal != last_day_ordinal) {
		last_day_ordinal = day_ordinal;
		const char *blessing_text = find_special_message(&local_time);
		if (blessing_text == NULL) {
			blessing_text = daily_lines[day_ordinal % (sizeof(daily_lines) / sizeof(daily_lines[0]))];
		}
		lv_label_set_text(blessing_shadow, blessing_text);
		lv_label_set_text(blessing_label, blessing_text);
	}

	int minutes = local_time.tm_hour * 60 + local_time.tm_min;
	bool daytime = minutes >= 360 && minutes < 1080;
	int dark_opa;
	if (minutes < 360 || minutes >= 1440) {
		dark_opa = LV_OPA_COVER;
	} else if (minutes < 720) {
		dark_opa = LV_OPA_COVER - (minutes - 360) * LV_OPA_COVER / 360;
	} else if (minutes < 1080) {
		dark_opa = (minutes - 720) * LV_OPA_50 / 360;
	} else {
		dark_opa = LV_OPA_50 + (minutes - 1080) * LV_OPA_50 / 360;
	}
	lv_obj_set_style_opa(dark_background, (lv_opa_t)dark_opa, 0);
	lv_obj_set_style_opa(sun_object, dark_opa < LV_OPA_50 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_opa(moon_object, dark_opa >= LV_OPA_50 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

	float progress;
	if (daytime) {
		progress = (float)(minutes - 360) / 720.0f;
	} else if (minutes >= 1080) {
		progress = (float)(minutes - 1080) / 720.0f;
	} else {
		progress = (float)(minutes + 360) / 720.0f;
	}
	int object_x = (int)(progress * 176.0f);
	int object_y = 32 - (int)(sinf(progress * 3.14159f) * 28.0f);
	lv_obj_set_pos(daytime ? sun_object : moon_object, object_x, object_y);
}

static lv_obj_t *create_text_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
		lv_coord_t x, lv_coord_t y, lv_coord_t width, lv_text_align_t align)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, color, 0);
	lv_obj_set_style_text_align(label, align, 0);
	lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
	lv_obj_set_width(label, width);
	lv_obj_set_pos(label, x, y);
	return label;
}

/* 主本落在方格的 (x+r, y+r)，所以要让文字中心钉在 (X, Y)，
 * 传进来的起点得是 (X-r, Y-r)。日期/星期/结婚提示 r=1，时间 r=2。 */
static outlined_text_t create_outlined_text(lv_obj_t *parent, const lv_font_t *font,
		lv_coord_t x, lv_coord_t y, lv_coord_t width, lv_text_align_t align, uint8_t radius)
{
	assert(radius >= 1 && radius <= OUTLINE_MAX_RADIUS);
	outlined_text_t ot = {0};
	const int span = 2 * radius + 1;
	/* 黑色副本铺满整个 span×span 方格，只跳过正中心那一格（留给白色主本） */
	for (int dy = 0; dy < span; dy++) {
		for (int dx = 0; dx < span; dx++) {
			if (dx == radius && dy == radius) {
				continue;
			}
			ot.layers[ot.n++] = create_text_label(parent, font, lv_color_black(),
					x + dx, y + dy, width, align);
		}
	}
	/* 白色主本最后画，盖在黑色方格正中心 */
	ot.layers[ot.n++] = create_text_label(parent, font, lv_color_white(),
			x + radius, y + radius, width, align);
	return ot;
}

static void set_outlined_text(outlined_text_t *ot, const char *txt)
{
	for (int i = 0; i < ot->n; i++) {
		lv_label_set_text(ot->layers[i], txt);
	}
}

/* 大爱心心跳：value 在 256..282 内做线性缩放，绕 pivot(40,36) 在原地呼吸。
 * 用 lv_img_set_zoom 会进 LVGL transform 离屏层，C3 上无 PSRAM 时会卡帧（已知）。 */
static void heart_zoom_animation(void *object, int32_t value)
{
	lv_img_set_zoom(object, (uint16_t)value);
}

static void create_ui(void)
{
	screen = lv_scr_act();
	lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	light_background = lv_img_create(screen);
	dark_background = lv_img_create(screen);
	lv_img_set_src(light_background, &light_scene_image);
	lv_img_set_src(dark_background, &dark_scene_image);
	lv_obj_set_pos(light_background, 0, 0);
	lv_obj_set_pos(dark_background, 0, 0);
	lv_obj_move_background(light_background);

	sun_object = lv_img_create(screen);
	moon_object = lv_img_create(screen);
	heart_object = lv_img_create(screen);
	lv_img_set_src(heart_object, &heart_image);
	lv_obj_set_pos(heart_object, HEART_POS_X, HEART_POS_Y);
	lv_obj_set_style_opa(heart_object, LV_OPA_COVER, 0);
	/* 大爱心心跳：lv_anim 256..282 线性缩放，绕 pivot(40,36) 在原地呼吸。 */
	lv_img_set_pivot(heart_object, 40, 36);
	lv_anim_t heart_animation;
	lv_anim_init(&heart_animation);
	lv_anim_set_var(&heart_animation, heart_object);
	lv_anim_set_exec_cb(&heart_animation, heart_zoom_animation);
	lv_anim_set_values(&heart_animation, 256, 282);
	lv_anim_set_time(&heart_animation, 1000);
	lv_anim_set_playback_time(&heart_animation, 1000);
	lv_anim_set_repeat_count(&heart_animation, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&heart_animation);

	date_text_ot = create_outlined_text(screen, &my_font_18, DATE_X, ROW_DATE_Y, DATE_WIDTH, LV_TEXT_ALIGN_RIGHT, 1);
	weekday_text_ot = create_outlined_text(screen, &my_font_18, WEEKDAY_X, ROW_DATE_Y, WEEKDAY_WIDTH, LV_TEXT_ALIGN_LEFT, 1);
	/* 时间用 2px 全包描边：起点回退 r，主本才落在 (120, ROW_TIME_Y) 上 */
	time_text_ot = create_outlined_text(screen, &lv_font_montserrat_48,
			-TIME_OUTLINE_R, ROW_TIME_Y - TIME_OUTLINE_R,
			DISPLAY_WIDTH, LV_TEXT_ALIGN_CENTER, TIME_OUTLINE_R);
	marriage_text_ot = create_outlined_text(screen, &my_font_18, 0, ROW_MARRIAGE_Y, DISPLAY_WIDTH, LV_TEXT_ALIGN_CENTER, 1);
	/* 「第 / 天数 / 天」仍是三个独立标签：「第」「天」留在爱心外面，只有天数压在爱心上。
	 * 三者共用 ROW_DAYS_Y，基线一致，于是整体在垂直方向与爱心中心同一条线。 */
	days_prefix_shadow = create_text_label(screen, &my_font, lv_color_black(), DAYS_PREFIX_X + 1, ROW_DAYS_Y + 1, DAYS_PREFIX_W, LV_TEXT_ALIGN_CENTER);
	days_prefix_label = create_text_label(screen, &my_font, lv_color_white(), DAYS_PREFIX_X, ROW_DAYS_Y, DAYS_PREFIX_W, LV_TEXT_ALIGN_CENTER);
	/* 天数按需求不加黑边：只叠两层白字做加粗，压在爱心上靠白色本身与底色区分 */
	days_label = create_text_label(screen, &my_font, lv_color_white(), DAYS_X, ROW_DAYS_Y, DAYS_W, LV_TEXT_ALIGN_CENTER);
	days_label_bold = create_text_label(screen, &my_font, lv_color_white(), DAYS_X + 1, ROW_DAYS_Y + 1, DAYS_W, LV_TEXT_ALIGN_CENTER);
	days_suffix_shadow = create_text_label(screen, &my_font, lv_color_black(), DAYS_SUFFIX_X + 1, ROW_DAYS_Y + 1, DAYS_SUFFIX_W, LV_TEXT_ALIGN_CENTER);
	days_suffix_label = create_text_label(screen, &my_font, lv_color_white(), DAYS_SUFFIX_X, ROW_DAYS_Y, DAYS_SUFFIX_W, LV_TEXT_ALIGN_CENTER);
	blessing_shadow = create_text_label(screen, &my_font_13, lv_color_black(), 1, ROW_BLESSING_Y + 1, DISPLAY_WIDTH, LV_TEXT_ALIGN_CENTER);
	blessing_label = create_text_label(screen, &my_font_13, lv_color_white(), 0, ROW_BLESSING_Y, DISPLAY_WIDTH, LV_TEXT_ALIGN_CENTER);
	lv_label_set_long_mode(blessing_shadow, LV_LABEL_LONG_CLIP);
	lv_label_set_long_mode(blessing_label, LV_LABEL_LONG_CLIP);
	lv_label_set_text(days_prefix_shadow, "第");
	lv_label_set_text(days_prefix_label, "第");
	set_outlined_text(&marriage_text_ot, "今天是我们结婚的");
	lv_label_set_text(days_suffix_shadow, "天");
	lv_label_set_text(days_suffix_label, "天");

	lv_img_set_src(sun_object, &sun_image);
	lv_img_set_src(moon_object, &moon_image);
	lv_obj_set_pos(sun_object, 8, 64);
	lv_obj_set_pos(moon_object, 8, 64);
	/* outlined 8 邻居：8 个黑描边先画，1 个白主本后画盖在中心 */
	for (int i = 0; i < date_text_ot.n; i++) lv_obj_move_foreground(date_text_ot.layers[i]);
	for (int i = 0; i < weekday_text_ot.n; i++) lv_obj_move_foreground(weekday_text_ot.layers[i]);
	for (int i = 0; i < time_text_ot.n; i++) lv_obj_move_foreground(time_text_ot.layers[i]);
	for (int i = 0; i < marriage_text_ot.n; i++) lv_obj_move_foreground(marriage_text_ot.layers[i]);
	lv_obj_move_foreground(days_prefix_shadow);
	lv_obj_move_foreground(days_prefix_label);
	lv_obj_move_foreground(days_label);
	lv_obj_move_foreground(days_label_bold);
	lv_obj_move_foreground(days_suffix_shadow);
	lv_obj_move_foreground(days_suffix_label);
	lv_obj_move_foreground(blessing_shadow);
	lv_obj_move_foreground(blessing_label);

	lv_timer_create(update_scene, 1000, NULL);
	update_scene(NULL);
}

void app_main(void)
{
	esp_err_t nvs_error = nvs_flash_init();
	if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES || nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		nvs_error = nvs_flash_init();
	}
	ESP_ERROR_CHECK(nvs_error);
	initialize_rtc();

	/* Wi-Fi 凭据优先用 NVS 里存过的（配网网页改过），没有才用 Kconfig 默认值 */
	load_wifi_credentials();
	bool connected = connect_wifi();
	wifi_connected = connected;
	synchronize_time();
	initialize_display();
	create_ui();
	/* 定时校时任务：每天 0 点跑一轮，独立任务避免阻塞 UI */
	xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 2, NULL);
	/* 一个都连不上就开配网热点，手机连上后浏览器打开 192.168.4.1 改 Wi-Fi */
	if (!connected) {
		xTaskCreate(provisioning_task, "provisioning", 6144, NULL, 2, NULL);
	}

	while (true) {
		uint32_t delay_ms = lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(delay_ms < 5 ? 5 : delay_ms));
	}
}
