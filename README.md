# ESP32-C3 蓝牙靠近告警器

一个给 ESP32-C3 小板子用的蓝牙监测小程序。板子自己开热点，手机连上后打开网页，就能看到附近正在广播的 BLE 设备。网页里勾选要盯的设备以后，只要它再次出现在附近，小屏会显示大概距离，蜂鸣器会滴滴响，小灯也会闪。

这个项目主要是给带 0.42 寸 OLED 的 ESP32-C3 小开发板用的，代码里已经带了一个很小的 SSD1306 驱动，不用再额外装 OLED 库。

## 能做什么

- 扫描附近 BLE 设备，网页里能看到信号强度、估算距离、广播名称、MAC、厂商数据等。
- 可以在网页上勾选一个或多个目标设备。
- 目标出现时，GPIO1 上的无源蜂鸣器会发出滴滴告警。
- GPIO2 上的小灯泡或 LED 会跟着闪。
- OLED 会显示当前目标的大概距离。
- 会尽量识别常见品牌，比如 Apple、小米、华为、vivo、OPPO、三星、Google 等。

## 硬件接线

| 模块 | ESP32-C3 引脚 |
| --- | --- |
| 无源蜂鸣器 | GPIO1 |
| 小灯泡 / LED 模块 | GPIO2 |
| OLED SDA | GPIO5 |
| OLED SCL | GPIO6 |
| OLED 地址 | 0x3C |

如果你的屏幕 I2C 引脚不一样，修改 `esp32c3_ble_target_alarm.ino` 顶部：

```cpp
#define OLED_SDA_PIN 5
#define OLED_SCL_PIN 6
```

如果灯的亮灭逻辑反了，把下面这一行改成 `1`：

```cpp
#define LAMP_ACTIVE_LOW 0
```

## 烧录

1. 打开 Arduino IDE。
2. 安装 ESP32 Arduino core。
3. 选择 `ESP32C3 Dev Module` 或你的 ESP32-C3 开发板型号。
4. 打开并烧录 `esp32c3_ble_target_alarm.ino`。

我这里用 `esp32:esp32:esp32c3` 编译过了。这个程序同时用了 BLE、Wi-Fi AP 和 WebServer，体积会比较大，默认分区下已经接近上限。如果 Arduino IDE 提示空间不够，换一个更大的 APP 分区就行。

## 使用

1. 烧录后等待开发板启动。
2. 手机或电脑连接热点：
   - Wi-Fi：`ESP32C3-BLE`
   - 密码：`12345678`
3. 浏览器打开 `http://192.168.4.1`。
4. 在网页表格中勾选需要监测的蓝牙设备。
5. 被勾选设备出现在附近时：
   - OLED 显示估算距离。
   - GPIO1 无源蜂鸣器响起滴滴告警。
   - GPIO2 小灯泡或 LED 闪烁。

## 距离估算

距离是用 RSSI 粗略算出来的，不是精确测距。手机放进口袋、隔一堵墙、天线方向不同，数字都会跳。想让它更贴近你的现场，可以改这几个值：

```cpp
#define RSSI_AT_1M -59
#define PATH_LOSS_N 2.2f
#define ALERT_MIN_RSSI -98
```

## 设备识别说明

品牌识别不是百分百准确。程序会先看 BLE 厂商数据里的 Company ID，读不到时再看广播名称里的关键词。很多手机、耳机、手环不会把真实型号直接广播出来，所以网页能显示的是“尽量猜到的品牌和类型”，不是官方设备清单。

## 文件

- `esp32c3_ble_target_alarm.ino`：主程序，OLED、BLE 扫描、网页和告警逻辑都在这里。
