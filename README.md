# ESP32-C3 BLE Target Alarm

ESP32-C3 蓝牙监测告警器。开发板会开启 Wi-Fi 热点和网页控制台，扫描附近 BLE 设备，显示 RSSI、估算距离、品牌、设备类型、广播名称、MAC 地址、厂商数据；在网页勾选目标设备后，只要该设备再次出现在附近，小屏幕会显示估算距离，GPIO1 无源蜂鸣器会发出“滴～滴～滴”的告警声，GPIO2 小灯泡或 LED 会闪烁。

## 功能

- ESP32-C3 自建热点，无需路由器。
- 网页显示附近 BLE 设备详情。
- 支持勾选一个或多个蓝牙目标进行监测。
- 目标出现时蜂鸣器和灯光告警。
- 72x40 SSD1306 小屏显示目标状态和估算距离。
- 对常见品牌做启发式识别：Apple、Xiaomi/Redmi/Mijia、Huawei/Honor、vivo/iQOO、OPPO/OnePlus/realme、Samsung、Google 等。

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

本项目已用 `esp32:esp32:esp32c3` 编译通过。BLE + Wi-Fi AP + WebServer 占用空间较大，默认分区下程序大小接近上限。如果 Arduino IDE 提示空间不足，请选择更大的 APP 分区或更合适的 ESP32-C3 板型配置。

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

BLE 距离由 RSSI 粗略估算，不是精确测距。手机发射功率、人体遮挡、墙体、天线方向都会导致距离跳动。需要现场校准时可调整：

```cpp
#define RSSI_AT_1M -59
#define PATH_LOSS_N 2.2f
#define ALERT_MIN_RSSI -98
```

## 设备识别说明

品牌识别优先读取 BLE 厂商数据中的 Company ID；读取不到时，会用广播名称关键词兜底。由于 iOS、Android 和很多耳机/手环会隐藏真实型号，网页会尽量显示可识别的品牌和类型，但不能保证所有设备都能显示准确型号。

## 文件

- `esp32c3_ble_target_alarm.ino`：Arduino 主程序，包含 OLED 驱动、BLE 扫描、网页控制台和告警逻辑。
