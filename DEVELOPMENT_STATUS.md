# 结婚纪念日桌面时钟 · 开发状态

最后更新：2026-09-10

> **使用手册见 [README.md](README.md)**（配网、改图、改文案、故障排查）。
> 本文档面向开发：实现细节、踩过的坑、脚本用法。

## 当前结论

功能已完整，2026-09-10 实机验证通过：硬件初始化、联网校时、DS3231 断网走时、LVGL 显示、图片资源、UI 排版、每日 0 点自动校时、SoftAP 网页配网（开热点 → 填表 → 重启连接全流程跑通）。

**改动 Wi-Fi 不再需要重新烧录**——所有已配置网络都连不上时，设备会自己开一个 `ClockSetup` 热点，手机连上浏览器打开 `192.168.4.1` 填表即可，保存后自动重启。热点是临时的，10/30 分钟无操作自动关闭。

代码已清理：无死代码（未用宏、失效脚本、历史 `.bak` 备份均已删除）。

## 硬件

| 项目 | 配置 |
|------|------|
| 主控 | ESP32-C3（无 PSRAM） |
| 框架 | ESP-IDF 5.5.5 |
| 图形 | LVGL 8.4.0（本地组件，非 IDF 自带） |
| 屏幕 | ILI9341 240×320 竖屏 SPI |
| 屏幕 SPI | SCLK 2 / MOSI 3 / CS 7 / DC 10 / RST 13 |
| 背光 | GPIO5，高电平点亮 |
| RTC | DS3231，I2C SDA 19 / SCL 18，400kHz |
| Flash | 4MB，factory 分区 3MB |

> GPIO18/19 可能与 ESP32-C3 内置 USB Serial/JTAG 冲突，调试时注意。

## 网络与时间

- Wi-Fi 最多 3 组，按顺序尝试。**凭据优先读 NVS，读不到才用 Kconfig 编译的默认值**。
- 启动流程：NVS → 尝试连接 → NTP 校时（时区 `CST-8`）→ 成功则回写 DS3231。
- **每日 0 点自动再校时一次**（独立任务，不阻塞 UI）。
- 无网络时回退读 DS3231，断网也能正常走时。
- 所有网络都连不上 → 自动开配网热点（见下文），**但不会一直开着**。

### 网页配网使用步骤

1. 设备上电，如果连不上任何已存的 Wi-Fi，屏幕语录区会显示「请连接热点 ClockSetup」。
2. 手机或电脑连上开放热点 `ClockSetup`（无密码）。手机可能提示"此网络无法访问互联网"，选择**保持连接**。
3. 浏览器打开 `http://192.168.4.1`。
4. 填写最多 3 组 Wi-Fi 名称和密码，用不到的行留空。
5. 点「保存并重启」→ 写入 NVS → 设备自动重启并用新配置连接。

### 热点什么时候关（长期断网的行为）

热点是**临时的**，三种情况下自动关闭，关闭后屏幕恢复正常每日语录：

| 触发条件 | 时间 | 说明 |
|---|---|---|
| 一直没设备连进来 | 10 分钟 | 最常见。没人在配网就没必要开着 |
| 有设备连着但一直不提交 | 30 分钟 | 兜底，防止手机挂着不走 |
| 网络自己恢复了 | 立即 | 每 60 秒重试一次已保存的 Wi-Fi，连上就关热点并顺手校时 |

关闭后设备就是一块普通时钟：时间由 DS3231 走，每日语录正常显示，不再常驻开放热点。
**需要重新配网时重启一次设备**（重新上电）就会再开一轮热点。

配网只在连不上网时启动，平时不占内存、不开热点。

### 配网实现要点（改这块代码前先看）

- **页面 buffer 必须是 `static`**。`HTTPD_DEFAULT_CONFIG()` 的任务栈只有 4096，页面 2300 字节压在栈上再加 `snprintf` 开销会直接溢出重启。现已改静态 + `config.stack_size = 8192`。
- **HTML 是 `snprintf` 的格式串**（6 个 `%s` 填 SSID/密码），CSS 里字面的 `%` 必须写成 `%%`，否则 `-Werror=format` 编译失败。
- **`try_wifi_network()` 有 `wifi_connect_lock` 互斥**：配网任务和每日校时任务都会重连，不能同时调 `esp_wifi_connect`。
- **LVGL 不是线程安全的**：配网任务只置 `provisioning_active` 标志位，屏幕文本一律在 `update_scene()`（主循环）里改。
- 语录换日标记 `last_day_ordinal` 是文件级变量，配网结束时置 `-1` 才能让语录区恢复。
- 关热点走 `httpd_stop()` → `esp_wifi_set_mode(WIFI_MODE_STA)`；切模式会重启 Wi-Fi 接口，所以恢复网络时要再 `ensure_wifi_connection()` 一次。

## 已实现功能

- NVS 初始化 + Wi-Fi 凭据持久化（namespace `clock_wifi`）。
- 多组 Wi-Fi 连接、断线重连、日志输出。
- SoftAP 网页配网，热点空闲 10 分钟 / 最长 30 分钟自动关闭并恢复语录。
- NTP 校时 + DS3231 读写 + 每日 0 点定时校时。
- ILI9341 原生 SPI 驱动（不依赖 LVGL 屏驱）。
- LVGL 240×40 行局部双缓冲。
- 日期、星期、`HH:MM` 时间显示。
- 「今天是我们结婚的」固定提示。
- 「第 / 天数 / 天」三 label 包围爱心，天数压在爱心正中。
- 公历固定节日 + 农历节日语录，优先于日常语录。
- 31 条日常语录按天轮换（每天本地 0 点换，重启后仍是同一条）。
- 太阳 / 月亮沿弧线移动（固定 06:00–18:00 日夜区间，非真实天文数据）。
- 大爱心心跳动画（`lv_anim` 256→282，周期 2 秒）。

## 版面布局

### 垂直

| 区域 | Y | 字体 | 描边 |
|------|---|------|------|
| 日期 / 星期 | `ROW_DATE_Y=19` | SimHei 18 | r=1 全包 |
| 时间 `HH:MM` | `ROW_TIME_Y=37` | Montserrat 48 | r=1 全包 |
| 「今天是我们结婚的」 | `ROW_MARRIAGE_Y=95` | SimHei 18 | r=1 全包 |
| 「第 / 天数 / 天」 | `ROW_DAYS_Y=135` | SimHei 25 | 「第/天」1px；天数无描边、双层白字加粗 |
| 每日语录 | `ROW_BLESSING_Y=189` | SimHei 13 | 单层 1px |

### 水平

- 日期 right-align 右端 x=153，星期 left-align 左端 x=159，中间 6px，整体 x=27..213，中心 120。
- 时间 / 结婚提示 / 天数 / 语录 都在 x=120 中线。
- 「第」x=42..70，「天数」x=86..154，「天」x=168..196。
- 爱心 80×72 放在 (80,108)，墨迹占 x=88..151 / y=122..175，**视觉中心 (120,147)** 而非几何中心 (120,144)。

### 描边机制

统一走 `create_outlined_text()`：黑色副本铺满 (2r+1)×(2r+1) 方格、白色主本压正中心，四边等厚。日期/星期/结婚提示/时间都是 r=1（8 黑 + 1 白）。时间文案只在分钟跳变时写，避免每秒重绘 9 层 48px 大字。

## 图片资源

素材在 `main/assert/`：

| 文件 | 尺寸 | 说明 |
|------|------|------|
| `light.png` | 240×320 | 白天天空背景 |
| `dark.png` | 240×320 | 夜晚天空背景 |
| `characters.png` | 1536×2048 | 底部人物，透明底（2026-09-10 换新，含狗和猫） |
| `heart.png` | 80×72 | 大爱心 |
| `sun.png` | 64×64 | 太阳 |
| `moon.png` | 64×64 | 月亮 |

处理流程：`characters.png` 缩到 240×320 → 分别与 `light.png` / `dark.png` 合成 → 转不透明 RGB565 → `main/light_scene_image.c` / `dark_scene_image.c`。太阳、月亮、爱心保持独立透明资源以便动画。

**换人物图只需两步**：替换 `main/assert/characters.png`，然后 `python tools/rebuild_scene.py`。

## 字体

| 字体 | 字号 | 用途 |
|------|------|------|
| `my_font.c` | 25px | 「第 / 天数 / 天」 |
| `my_font_18.c` | 18px | 日期 / 星期 / 结婚提示 |
| `my_font_13.c` | 13px | 每日语录 |
| `lv_font_montserrat_48` | 48px | 时间数字 |

**新增中文文案前必须确认字符已在对应字库的 cmap 里**，否则屏幕上是空白，很难看出来。用 `tools/check_font_coverage.py` 检查，用 `tools/longest_lines.py` 量宽度。

## 工具脚本

| 脚本 | 用途 |
|------|------|
| `tools/rebuild_scene.py` | **换人物图后重建日夜两张场景底图**，并输出 3 倍预览图供验收 |
| `tools/gen_lvgl_font.py` | TTF → LVGL 8.4 4bpp C 字库 |
| `tools/verify_font.py` | 离线验证「码点 → 字形」映射（被 `check_font_coverage.py` 依赖） |
| `tools/check_font_coverage.py` | 检查字库是否覆盖所有会显示的文案，**改文案后必跑** |
| `tools/longest_lines.py` | 算每条语录宽度并排序，**加文案前必跑**（超 240px 会被静默裁掉右侧） |
| `tools/prepare_icon.py` | 黑底大图标 → 透明小素材 |
| `tools/png_to_lvgl.py` | 单张 PNG → LVGL RGB565 C 资源 |

Python 用系统 Python 3.12（有 PIL），托管 Python 3.13 没装 PIL。

## 重要文件

- `main/lvgl_stable.c`：全部业务逻辑（硬件初始化、校时、UI、动画、配网）。
- `main/lunar_special.c`：农历节日表，6 节日 × 75 年（2026–2100）= 450 条。
- `main/light_scene_image.c` / `dark_scene_image.c`：日夜合成背景（各约 954KB）。
- `main/Kconfig`：Wi-Fi 默认值、GPIO、RTC、纪念日日期。
- `components/lv_conf.h`：**唯一有效的 LVGL 配置源**（`LV_CONF_INCLUDE_SIMPLE` 生效，`sdkconfig` 里的 `CONFIG_LV_*` 不生效）。

## 已知问题与风险

### LVGL 渲染陷阱（重要）

- **不要用 transform 做大动画**。`lv_img_set_angle` / `set_zoom` / `set_pivot` 或文字 `transform_zoom != 256` 会让 LVGL 判定为 `LV_LAYER_TYPE_TRANSFORM`，必须一次性申请整块离屏图层；C3 无 PSRAM 时容易失败导致对象静默不画或卡帧。需要旋转/缩放请**预烘焙多帧 + 运行时切 `lv_img_set_src`**。
  > 当前大爱心心跳仍在用 `lv_img_set_zoom`（transform 层），实测有轻微卡顿，2026-09-09 评估后决定保留此状态。
- `lv_obj_set_style_opa` / `lv_obj_set_pos` 即使值没变也会 invalidate 该矩形。对 240×320 的大背景尤其要避免每秒无脑 set，否则等于每秒重画全屏。需要时加"值守卫"。
- `lv_anim` 挂在 `LV_DISP_DEF_REFR_PERIOD`（默认 30Hz）上，高频动画会累积脏区。

### 图像处理陷阱

- **透明 PNG 缩放绝不能用亮度当 alpha**。`tools/prepare_icon.py` 的 `alpha = max(R,G,B)` 只适用于**黑底图**（本质是颜色×alpha 的预乘结果）。正常透明 PNG 自带真实 alpha（不透明处恒为 255），若误用亮度，黑头发/深色衣服的 alpha 会掉到十几，合成后整体严重褪色。`tools/rebuild_scene.py` 已按「判断 `alpha.getextrema()[0] < 255`」分两种处理。
- 缩放要在**预乘空间**做（LANCZOS 直接插值非预乘 RGBA 会在边缘混入黑边），最后反预乘还原。

### 其他

- 屏幕最底部一行有一条固定横线，GRAM 上电残留所致，影响不大，未处理。
- `main/lvgl_stable.c` 顶部 `TEST_FORCE_DATE_ON` 当前为 `0`（正常逻辑）。设为 `1` 可强制显示指定日期的语录用于抽查排版，用完改回 `0`。
- 太阳/月亮是固定日夜区间，不是真实日出日落。
- **本机没有 IDF 的 Python 虚拟环境**（`C:/Users/lenovo/.espressif/python_env` 不存在），改完代码无法本地编译验证，只能人工核对，编译错误要靠用户 build 才能暴露。

### 两个容易误判的行为

- **擦 NVS ≠ 清空 Wi-Fi**。`esptool.py erase_region 0x9000 0x6000` 的效果是"回到 Kconfig 编译默认值"，设备照样会连上默认网络。想让它连不上，得改路由器（关 SSID 广播/改密码/MAC 拉黑）或把默认 SSID 清空后重烧。
- **手机一连热点设备重启**，先看串口有没有 `Brownout detector was triggered`。开热点发射电流比 STA 大，弱电源会掉电重启，跟代码无关；代码侧的同类问题（httpd 栈溢出）已修。

## 构建与烧录

用 CMD（已配好 `export.bat`）：

```cmd
idf.py build
idf.py -p COM4 flash
```

从 IDE 的 PowerShell 构建需要手动加载环境：

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.5.5'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:PATH = 'D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;' + $env:PATH
. 'D:\Espressif\frameworks\esp-idf-v5.5.5\export.ps1'
idf.py build
```

构建卡住时：结束残留 `ninja.exe` / `ccache.exe`，删除 `build/.ninja_lock`，重新 build。

## 待办

- 观察每日 0 点自动校时是否正常触发（串口日志 `scheduled time sync at 00:00`）。
- 可选增强：联网状态下也起 HTTP server（或加 mDNS），这样不用断网也能改 Wi-Fi。
- 可选增强：配网网页加「恢复默认」按钮，省得接串口擦 NVS。
