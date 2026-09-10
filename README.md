# 结婚纪念日桌面时钟 · 使用手册

一台摆在桌上的小相框时钟：显示日期、时间、结婚第 N 天，每天换一句语录，日夜背景和日月位置随时间变化。联网时自动校时，断网时靠 DS3231 继续走。

- 使用说明在本文档
- 技术实现、坑与脚本说明见 [DEVELOPMENT_STATUS.md](DEVELOPMENT_STATUS.md)

## 一、屏幕显示什么

| 区域 | 内容 |
|------|------|
| 顶部 | 日期 + 星期 |
| 中间大字 | 当前时间 `HH:MM` |
| 中部 | 「今天是我们结婚的」 |
| 爱心 | 「第 **N** 天」，天数压在爱心正中 |
| 底部 | 每日语录（公历/农历节日优先，否则按天轮换） |
| 背景 | 白天/夜晚两张底图，太阳月亮沿弧线移动 |

日夜切换是固定的 06:00–18:00，不是真实天文日出日落。

## 二、硬件与接线

| 项目 | 配置 |
|------|------|
| 主控 | ESP32-C3（无 PSRAM） |
| 屏幕 | ILI9341 240×320 竖屏 SPI |
| 屏幕 SPI | SCLK 2 / MOSI 3 / CS 7 / DC 10 / RST 13 |
| 背光 | GPIO5，高电平点亮 |
| RTC | DS3231，SDA 19 / SCL 18，400kHz |
| Flash | 4MB |

改引脚在 `main/Kconfig`（`idf.py menuconfig` → Clock display）。

## 三、编译与烧录

用配好环境的 ESP-IDF 命令行：

```cmd
idf.py build
idf.py -p COM4 flash
```

COM 口按实际改。换过 `main/CMakeLists.txt` 或 `sdkconfig` 后 cmake 会重新配置，第一次会慢一点。

## 四、Wi-Fi 配网

Wi-Fi 账号密码存在 Flash 的 NVS 里，**改 Wi-Fi 不用重新烧录程序**。

### 4.1 什么时候会开热点

设备上电后按顺序尝试 3 组 Wi-Fi，**一组都连不上**才开配网热点 `ClockSetup`（无密码）。
如果连上了，热点不会出现——这时想改 Wi-Fi 见 4.3。

### 4.2 配网步骤

1. 屏幕语录区显示「请连接热点 ClockSetup」。
2. 手机连上开放热点 `ClockSetup`。安卓可能提示"此网络无法访问互联网"，选**保持连接**。
3. 浏览器打开 `http://192.168.4.1`。
4. 填最多 3 组 Wi-Fi 名称和密码，用不到的行留空。
5. 点「保存并重启」→ 写入 NVS → 自动重启并用新配置连接。

### 4.3 现在连着网，想改 Wi-Fi 怎么办

得先让设备连不上原有网络，它才会开热点。任选一种（都不用改代码）：

- **推荐**：路由器后台临时关闭 SSID 广播（隐藏网络）。已连的设备一般不掉线，但设备重启后扫描不到这个名字 → 开热点。配完再把广播打开。
- 临时改家里 Wi-Fi 密码 → 设备重启连不上 → 开热点 → 填新密码保存 → 密码就保持新的（已写进设备）。
- 路由器 MAC 过滤把这台设备拉黑，只有它连不上。

**一劳永逸的做法**（下次烧录时顺手改）：`idf.py menuconfig` 把三组默认 SSID 和密码全部留空。这样没有"编译内置"的账号可连，只要 NVS 里没有有效配置就必定开热点，以后换网、改密码都不用再想办法断网。

### 4.4 删除某组 Wi-Fi / 恢复默认

- **删掉某一组**：进配网页把那一行名称和密码都清空，保存。空 SSID 会被跳过。
  三行全清空 = 一条都不连，会一直开热点。
- **恢复到烧录时写死的默认账号**：擦掉 NVS 分区，重启后自动用默认值。
  ```
  esptool.py -p COMx erase_region 0x9000 0x6000
  ```
  注意这是"恢复默认"不是"清空"——默认值就是 Kconfig 里那几组账号，所以擦完它还是会连上家里的 Wi-Fi。

### 4.5 热点不会一直开着

| 情况 | 多久关 |
|------|--------|
| 一直没设备连进来 | 10 分钟 |
| 有设备连着但一直不提交 | 30 分钟 |
| 网络自己恢复了 | 立即（每 60 秒重试一次，连上就关热点并顺手校时） |

关掉后屏幕恢复正常语录，时间继续由 DS3231 走。**要再配网就重启一次设备**。
长期断网时它就是一块普通时钟，不常驻开放热点。

## 五、时间从哪来

1. 上电先读 DS3231 把系统时间拉起来（所以哪怕一直没网，时间也是对的）。
2. 连上 Wi-Fi 就跑一次 NTP（时区 `CST-8`），成功则回写 DS3231。
3. 之后**每天 0 点**自动再校一次；没网就回退读 DS3231。

DS3231 有纽扣电池，断电也走时。**时钟长期不准先怀疑电池没电**，其次检查有没有连上网（串口日志有 `time synchronized by NTP`）。

## 六、常见改动

| 想改什么 | 怎么做 |
|----------|--------|
| 纪念日日期 | `idf.py menuconfig` → Marriage year/month/day，或改 `main/Kconfig` 默认值 |
| 默认 Wi-Fi | `idf.py menuconfig` → Wi-Fi network 1/2/3 |
| 底部人物图 | 替换 `main/assert/characters.png`，然后 `python tools/rebuild_scene.py` |
| 每日语录 | `main/lvgl_stable.c` 里的 `daily_lines[]`（31 条按天轮换） |
| 节日语录 | `main/lunar_special.c` 的 `entries[]`（450 条，按 `YYYY-MM-DD` 精确匹配） |
| 字库 | 改完中文文案**必须**跑 `python tools/check_font_coverage.py`，缺字会显示空白 |

加中文文案后如果缺字，用系统 SimHei 重生成 13px 字库：

```cmd
python tools/gen_lvgl_font.py "C:/Windows/Fonts/simhei.ttf" 13 13 3 main/my_font_13.c my_font_13 main/lvgl_stable.c main/lunar_special.c
python tools/check_font_coverage.py
```

Python 用系统 Python 3.12（有 PIL），托管 Python 3.13 没装 PIL。

## 七、故障排查

| 现象 | 原因 / 处理 |
|------|-------------|
| 手机一连 `ClockSetup` 设备就重启 | 供电不足（开热点发射电流大）。换 5V/1A 以上适配器、换粗一点的线。串口若打出 `Brownout detector was triggered` 即为此 |
| 配网页面打不开 | 确认连的是 `ClockSetup` 而不是家里 Wi-Fi；地址是 `http://192.168.4.1`（不是 https）；热点 10 分钟后会关，重开要重启设备 |
| 擦了 NVS 还是连上默认 Wi-Fi | 正常，擦 NVS 的语义是"恢复默认"。要让它连不上得改路由器或用 4.3 的办法 |
| 文字显示成空白/方块 | 字库缺字，跑 `tools/check_font_coverage.py` 确认并按上一节重生成字库 |
| 屏幕最底部有一条横线 | 已知问题（GRAM 上电残留），影响不大，未处理 |
| 时间不准 | 看有没有连上网；DS3231 纽扣电池是否有电 |
| 语录宽度被截断 | 引号超 240px 会被静默裁掉右侧，加文案前跑 `tools/longest_lines.py` 量一下 |
| 改了字库/语录后编译报 `unknown conversion type character` | 配网 HTML 是 `snprintf` 的格式串，CSS 里字面的 `%` 必须写成 `%%` |
