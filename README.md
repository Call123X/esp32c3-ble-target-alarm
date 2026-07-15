# ESP32-C3 蓝牙靠近告警器

这是一个 ESP32-C3 用的蓝牙靠近提醒小项目。板子自己开热点，手机连上后打开网页，就能看到附近的 BLE 蓝牙设备。选中目标以后，只要目标再次出现在附近，就可以让蜂鸣器响、小灯闪、板载灯闪；带屏版本还会在 OLED 上显示大概距离。

现在已经改成小白更好用的 Arduino 结构：一个版本一个目录，每个目录里只有一个同名 `.ino`，没有共享头文件。直接打开对应目录里的 `.ino` 就能编译，不会再出现 `version_select.h: No such file or directory`。

## 版本

| 目录 | 适合场景 |
| --- | --- |
| `oled_basic/` | 带 OLED，小屏显示距离，网页勾选目标，GPIO1 蜂鸣器、GPIO2 外接灯、板载灯告警 |
| `oled_multi/` | 带 OLED，多目标多接口，不同蓝牙设备触发不同 GPIO |
| `nooled_basic/` | 不带屏幕，网页勾选目标，GPIO1 蜂鸣器、GPIO2 外接灯、板载灯告警 |
| `nooled_multi/` | 不带屏幕，多目标多接口，不同蓝牙设备触发不同 GPIO |

每个目录里只有一个文件：

```text
oled_basic/oled_basic.ino
oled_multi/oled_multi.ino
nooled_basic/nooled_basic.ino
nooled_multi/nooled_multi.ino
```

## 怎么用

1. 打开 Arduino IDE。
2. 选择你要用的版本目录，比如 `nooled_basic/nooled_basic.ino`。
3. 选择 ESP32-C3 开发板。
4. 编译、上传。
5. 手机或电脑连接热点：
   - Wi-Fi：`ESP32C3-BLE`
   - 密码：`12345678`
6. 浏览器打开 `http://192.168.4.1`。

基础版是在网页里勾选要监测的蓝牙设备。勾选记录会保存到 ESP32-C3，断电重启后还会继续监测。

多接口版需要先在代码顶部填好蓝牙 MAC 和 GPIO。示例长这样：

```cpp
TargetOutputRule targetRules[] = {
  {"aa:bb:cc:dd:ee:01", "target-1", 3, OUTPUT_LAMP, false, 0, false, -999, 0.0f, false, 0},
  {"aa:bb:cc:dd:ee:02", "target-2", 4, OUTPUT_PASSIVE_BUZZER, false, 2300, false, -999, 0.0f, false, 0},
};
```

把 `aa:bb:cc:dd:ee:01` 换成网页扫描到的蓝牙 MAC。`OUTPUT_LAMP` 是灯，`OUTPUT_PASSIVE_BUZZER` 是无源蜂鸣器。

## 接线

基础版默认接线：

| 模块 | ESP32-C3 引脚 |
| --- | --- |
| 无源蜂鸣器 | GPIO1 |
| 小灯泡 / LED 模块 | GPIO2 |
| 板载 LED | 默认 GPIO8，低电平点亮 |
| OLED SDA | GPIO5 |
| OLED SCL | GPIO6 |
| OLED 地址 | 0x3C |

无屏版本不用接 OLED。多接口版的 GPIO 在代码顶部的 `targetRules[]` 里配置。

如果你的板载灯不是 GPIO8，或者亮灭反了，改对应 `.ino` 顶部：

```cpp
#define BOARD_LED_PIN 8
#define BOARD_LED_ACTIVE_LOW 1
```

## 可以干什么

说白了，它就是一个“蓝牙设备靠近提醒器”。可以拿来做一些小提醒：

- 上课容易开小差时，提醒自己老师是不是走近了。
- 上班摸鱼时，提醒自己领导是不是快到附近了。
- 在家打游戏时，提醒家里人是不是突然回来了。
- 宿舍里不想被突然打断时，提醒室友是不是回来了。
- 给工具、钥匙、背包、蓝牙标签做靠近提示。
- 多接口版可以做到“这个设备亮这盏灯，那个设备响那个蜂鸣器”。

上面有些是玩笑场景，真正用的时候别越界。更推荐用来监测自己的设备、物品标签、工具标签，或者已经明确同意被监测的设备。

## 距离和名称

距离是用 RSSI 粗略估算出来的，不是精确测距。手机放进口袋、隔一堵墙、天线方向不同，数字都会跳。

蓝牙名称也不是每个设备都会广播。程序会尽量读取普通名称和中文 UTF-8 名称，但如果设备本身不广播名称，网页还是会显示“未广播名称”。

有些手机会使用随机蓝牙地址保护隐私。如果地址变了，之前勾选的目标可能就识别不到，需要重新勾选。

## 免责

这个项目只做 BLE 广播扫描和 RSSI 估算，不能精准定位，也不能保证一定能识别设备。请不要用于偷拍、跟踪、骚扰、侵犯隐私、绕过管理制度，或者其他违法违规用途。监测别人的手机、手环、耳机等设备前，应获得对方明确同意。接线、供电、外接蜂鸣器和灯的安全也需要自己确认。
