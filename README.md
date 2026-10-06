# Honor X1 Smart Home BLE Bridge

一个基于 **ESP32-C3** 的荣耀智慧屏 X1 智能家居 BLE Bridge。

当前同时支持：

- **Apple HomeKit** 原生 Television accessory
- iPhone 控制中心 **Apple TV Remote**
- **Home Assistant MQTT Discovery**（可选）
- 原装 **HDRC-BV1** 蓝牙遥控器中继
- 荣耀智慧屏待机 BLE 唤醒
- BLE Bond / Security / NVS 自动恢复

## 架构

```text
iPhone / Apple Home
        │ HomeKit
        │
        ├────────────────────────────┐
        │                            │
Home Assistant                      │
        │ MQTT                       │
        ▼                            ▼
      ┌──────────────────────────────────┐
      │             ESP32-C3             │
      │                                  │
      │ HomeKitAdapter   MqttAdapter     │
      │         \          /              │
      │          HonorBleBridge          │
      │               │                  │
      │      PhysicalRemote              │
      └───────────┬──────────────┬───────┘
                  │ BLE Central  │ BLE HID Peripheral
                  │              │
          ┌───────▼──────┐  ┌────▼───────────┐
          │ HDRC-BV1     │  │ 荣耀智慧屏 X1   │
          │ 原装遥控器    │  │                │
          └──────────────┘  └────────────────┘
```

HomeKit 和 Home Assistant 是两个独立的上层控制入口，最终都调用同一套 BLE 控制核心。

---

# 已实现功能

- HomeKit Television
- Apple TV Remote
- 上 / 下 / 左 / 右 / OK
- Back / Home / Menu
- Volume + / -
- Mute
- HomeKit / MQTT 开关机
- 关机宏：`Power → Right → OK`
- 特殊 BLE Wake ADV 唤醒电视
- TV BLE connection state → HomeKit / MQTT state
- 原装实体遥控器实时中继
- ESP32 重启后恢复原装遥控器 Bond
- 修复同地址重新配对后的 NimBLE Security Store 持久化问题
- GitHub Actions 自动生成 factory / update 固件

---

# 代码结构

```text
relay_bridge/
├── relay_bridge.ino        # setup / loop，仅负责组件编排
├── config.h                # 用户配置：遥控器 MAC、MQTT
├── HonorBleBridge.h/.cpp   # 电视 BLE HID、Wake、按键、状态
├── PhysicalRemote.h/.cpp   # 原装遥控器 BLE Central、中继、Bond/NVS
├── HomeKitAdapter.h/.cpp   # HomeSpan / Apple HomeKit
└── MqttAdapter.h/.cpp      # Home Assistant / MQTT Discovery
```

核心控制层与 HomeKit / MQTT 解耦。

如果以后改成 HTTP、Matter、ESPHome 或其他平台，不需要重新逆向电视 BLE 协议。

---

# 硬件

目标平台：

- ESP32-C3
- 4 MB Flash

已测试：

- ESP32-C3 Super Mini
- LuatOS / 合宙 CORE ESP32-C3
- LuatOS CORE ESP32-C3 minimal / Native USB

统一使用同一份固件。

Native USB 会占用 GPIO18 / GPIO19，本项目不依赖这两个 GPIO。

---

# 软件栈

| Component | Version |
| --- | --- |
| Arduino-ESP32 | 3.3.8 |
| ESP-IDF | 5.5.x |
| HomeSpan | 2.1.8 |
| NimBLE-Arduino | 2.5.0 |
| PubSubClient | 2.8 |

编译参数：

```text
esp32:esp32:esp32c3
PartitionScheme=min_spiffs
CDCOnBoot=cdc
FlashMode=dio
```

---

# 原装遥控器配置

编辑：

```text
relay_bridge/config.h
```

默认：

```cpp
#define ORIGINAL_REMOTE_MAC "18:70:3B:76:B8:45"
#define ORIGINAL_REMOTE_NAME "HDRC-BV1"
```

必须把 `ORIGINAL_REMOTE_MAC` 改成你自己的 HDRC-BV1 BLE Public Address。

## Android 获取 MAC

推荐使用 nRF Connect：

1. 打开 Scanner
2. 按一下遥控器按键；必要时按 **Home + Menu**
3. 找到 `HDRC-BV1`
4. 查看 Address
5. 优先确认 Address Type 为 `Public`

例如：

```text
18:70:3B:76:B8:45
```

iOS 通常不会向普通 BLE App 暴露真实设备 MAC，因此不推荐用 iPhone 查。

## Linux 获取 MAC

```bash
bluetoothctl
```

然后：

```text
power on
scan on
```

按遥控器按键，找到：

```text
Device 18:70:3B:76:B8:45 HDRC-BV1
```

前面的地址就是需要填写的 MAC。

---

# 为什么必须配置遥控器 MAC

HDRC-BV1 某些广播场景下，通过扫描 API 可能得到：

```text
00:00:00:00:00:00
```

但以下操作需要稳定 identity address：

- 自动重连
- Bond 判断
- OUR_SEC / PEER_SEC
- LTK / EDIV / RAND security record
- NVS 持久化

因此目标遥控器地址统一放在 `config.h`。

---

# Home Assistant

项目现在已经带有 **直接 MQTT 集成**。

默认关闭，不影响 HomeKit。

编辑：

```cpp
#define MQTT_ENABLED 1

#define MQTT_HOST "192.168.1.100"
#define MQTT_PORT 1883
#define MQTT_USERNAME "mqtt-user"
#define MQTT_PASSWORD "mqtt-password"
```

如果 Broker 无账号密码，可以保持：

```cpp
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""
```

默认 Topic：

```text
honor_x1/power/set
honor_x1/key/set
honor_x1/state
honor_x1/availability
```

Power：

```text
honor_x1/power/set
ON
OFF
```

按键：

```text
honor_x1/key/set

UP
DOWN
LEFT
RIGHT
OK
BACK
HOME
MENU
MUTE
VOLUME_UP
VOLUME_DOWN
POWER
```

设备连接 MQTT 后会向：

```text
homeassistant/...
```

发布 retained MQTT Discovery config。

Home Assistant 会自动发现：

- Power switch
- Up
- Down
- Left
- Right
- OK
- Back
- Home
- Menu
- Mute
- Volume Up
- Volume Down

HomeKit 和 MQTT 可以同时启用。

两边共享同一个真实电视状态：

```text
TV BLE connected    -> ON
TV BLE disconnected -> OFF
```

---

# HomeKit

HomeSpan Setup AP：

```text
SSID: HomeSpan-Setup
Password: homespan
```

默认 HomeKit pairing code：

```text
466-37-726
```

建议首次配置完成后修改。

## Apple TV Remote 映射

| HomeKit RemoteKey | Honor |
| --- | --- |
| 4 | Up |
| 5 | Down |
| 6 | Left |
| 7 | Right |
| 8 | OK |
| 9 | Back |
| 11 | Home |
| 15 | Menu |

VolumeSelector：

```text
0 = Volume +
1 = Volume -
```

---

# 开关机

## 开机

当 TV BLE 未连接时：

```text
HomeKit / MQTT ON
        ↓
特殊 Wake ADV
        ↓
电视唤醒
        ↓
TV BLE connected
        ↓
HomeKit Active / MQTT state = ON
```

## 关机

荣耀智慧屏 Power 键会先弹出菜单，因此 OFF 使用：

```text
Power
  ↓ 900 ms
Right
  ↓ 250 ms
OK
```

---

# BLE HID

Report ID 1：

```text
[modifier, reserved, key1, key2, key3, key4, key5, key6]
```

| Function | HID Key |
| --- | --- |
| Home | 0x4A |
| Power | 0x66 |
| Back | 0x29 |
| Voice | 0x75 |
| Menu | 0x76 |
| Up | 0x52 |
| Down | 0x51 |
| Left | 0x50 |
| Right | 0x4F |
| OK | 0x58 |
| Mute | 0x7F |
| Volume + | 0x80 |
| Volume - | 0x81 |

释放：

```text
00 00 00 00 00 00 00 00
```

Voice 还会启动厂商 Report ID `0x5A` 音频流；当前不转发语音音频。

---

# 模拟 HDRC-BV1 身份

BLE Name：

```text
HDRC-BV1
```

HID Service：

```text
0x1812
```

Appearance：

```text
0x03C1
```

Device Information：

```text
Manufacturer: Realtek BT
Model:        Model Nbr 0.9
Serial:       RTKBeeSerialNum
Firmware:     RTKBeeFirmwareRev
Hardware:     RTKBeeHardwareRev
Software:     RTKBeeSoftwareRev
```

PnP：

```text
Vendor ID Source: 1
Vendor ID:        0x7D02
Product ID:       0x0002
Product Version:  0x0003
```

---

# 电视唤醒广播

原装遥控器冷唤醒 Manufacturer Data：

```text
02 7D 03 00 <MAC> 01 01
```

ESP32 使用自己的 BLE identity：

```text
02 7D 03 00 <ESP32 BLE MAC> 01 01
```

同时使用：

```text
Flags:      0x04
Service:    0x1812
Appearance: 0x03C1
ADV Type:   legacy ADV_IND
```

Wake pulse 持续约 8 秒。

---

# 原装遥控器 Bond / NVS 修复

曾遇到：

```text
Home + Menu 重新配对 -> 正常
ESP32 重启           -> 遥控器无法恢复加密
HCI reason           -> 0x08 / 520
```

最终确认是同地址重新配对时，新的 security record 已更新到 RAM，但旧 NVS record 没有被覆盖。

当前实现会比较启动时和重新认证后的：

- LTK
- EDIV
- RAND
- IRK
- CSRK
- bond_count
- security flags

发现变化后强制刷新：

```text
OUR_SEC
PEER_SEC
```

从而保证重启后恢复正确密钥。

---

# 首次安装

推荐：

1. 修改 `ORIGINAL_REMOTE_MAC`
2. Flash factory firmware @ `0x0`
3. 配置 HomeSpan Wi-Fi
4. 添加到 Apple Home
5. 让电视与 ESP32 配对
6. 原装遥控器按住 **Home + Menu**
7. 等待遥控器 Bond
8. 重启验证自动恢复
9. 如需 Home Assistant，再配置 MQTT

---

# 固件

GitHub Actions 生成：

```text
honor-x1-smart-home-bridge.factory.bin
honor-x1-smart-home-bridge.update.bin
```

## Factory

刷写：

```text
0x0
```

用于：

- 新板
- 首次安装
- 完整重置

## Update

刷写：

```text
0x10000
```

日常更新使用。

**不要 erase flash。**

可以保留：

- Wi-Fi
- HomeKit pairing
- BLE Bonds
- NVS

---

# 串口

```text
115200 baud
```

---

# 已知限制

- 原装遥控器 MAC 需要在 `config.h` 中配置
- Voice Report `0x5A` 音频没有转发
- TV ON/OFF 状态目前以 BLE connection state 为代理
- HomeKit RemoteKey 不提供可靠长按语义
- 当前针对荣耀智慧屏 X1 / HDRC-BV1 实测
- Home Assistant MQTT 需要用户自己的 MQTT Broker

---

# 技术点

- ESP32-C3
- Arduino / ESP-IDF
- BLE GAP / GATT / HID
- BLE Central + Peripheral
- BLE Bond / LTK / EDIV / RAND
- NimBLE Security Store
- NVS persistence
- BLE advertising reverse engineering
- Apple HomeKit / HomeSpan
- Home Assistant
- MQTT Discovery
- PubSubClient
- Wi-Fi
- GitHub Actions CI
- Factory / Update firmware packaging
- State machine / reconnect recovery

---

# Disclaimer

这是个人逆向和智能家居实验项目。

项目与 Honor、Huawei、Apple、Espressif、Home Assistant 无官方关系。

所有品牌、商标归各自权利人所有。
