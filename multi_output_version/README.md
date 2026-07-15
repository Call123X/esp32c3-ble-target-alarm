# 多接口版本说明

这里放的是多目标、多输出接口版本：

```text
multi_output_version/esp32c3_ble_multi_output_alarm/esp32c3_ble_multi_output_alarm.ino
```

它和主目录里的基础版不一样。基础版适合“网页勾选目标，然后 GPIO1 蜂鸣器、GPIO2 小灯、板载灯一起告警”。多接口版适合提前写好多个目标和多个输出，比如：

- 设备 A 出现，GPIO3 的灯闪。
- 设备 B 出现，GPIO4 的无源蜂鸣器响。
- 设备 C 出现，GPIO7 的灯闪。
- 任意目标出现，ESP32-C3 板载灯也会闪。

## 怎么配置

打开 `esp32c3_ble_multi_output_alarm.ino`，找到这段：

```cpp
TargetOutputRule targetRules[] = {
  {"aa:bb:cc:dd:ee:01", "target-1", 3, OUTPUT_LAMP, false, 0, false, -999, 0.0f, false, 0},
  {"aa:bb:cc:dd:ee:02", "target-2", 4, OUTPUT_PASSIVE_BUZZER, false, 2300, false, -999, 0.0f, false, 0},
  {"aa:bb:cc:dd:ee:03", "target-3", 7, OUTPUT_LAMP, false, 0, false, -999, 0.0f, false, 0},
};
```

把前面的 MAC 地址换成网页里扫描到的蓝牙地址。后面的参数依次是：

| 参数 | 含义 |
| --- | --- |
| `"aa:bb:cc:dd:ee:01"` | 要监测的 BLE MAC 地址 |
| `"target-1"` | 备注名，方便自己看 |
| `3` | 输出 GPIO |
| `OUTPUT_LAMP` | 输出类型：灯/LED |
| `OUTPUT_PASSIVE_BUZZER` | 输出类型：无源蜂鸣器 |
| `false` | 是否低电平触发 |
| `2300` | 蜂鸣器频率，灯可以写 `0` |

## 几个版本怎么选

- `esp32c3_ble_target_alarm.ino`：基础版。网页勾选目标，GPIO1 蜂鸣器、GPIO2 小灯、板载灯告警，勾选记录会保存，重启还在。
- `multi_output_version/.../esp32c3_ble_multi_output_alarm.ino`：多接口版。适合给不同目标分配不同 GPIO，不同人/不同设备触发不同灯或蜂鸣器。

## 可以拿来做什么

这个东西本质上就是一个“BLE 靠近提醒器”。一些比较接地气的玩法：

- 上课容易开小差时，提醒自己老师是不是拿着设备走近了。
- 上班摸鱼时，提醒自己领导是不是快到附近了。
- 在家打游戏时，提醒家里人是不是回来了，赶紧切回正经画面。
- 宿舍里做点不想被突然打断的事时，提醒室友是不是回来了。
- 店铺、仓库、工作台上，提醒某个蓝牙标签、手环、工具或设备有没有靠近。
- 给不同工具贴 BLE 标签，不同工具靠近时亮不同颜色或不同位置的灯。

这些场景有点玩笑味，实际使用时要注意边界：更推荐用来监测自己的设备、公开同意的设备、工具标签、物品标签，而不是偷偷追踪别人。

## 免责和提醒

BLE RSSI 只能估算距离，不是精准定位。墙、人、口袋、手机功率、天线方向都会影响结果。

请不要把这个项目用于偷拍、跟踪、骚扰、侵犯隐私、绕过管理制度，或者任何违法违规用途。监测别人的手机、手环、耳机等设备前，应获得对方明确同意。作者只提供代码和硬件实验思路，使用者需要自行承担接线安全、合规和实际使用后果。
