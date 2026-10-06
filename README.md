# Honor X1 Smart Home BLE Bridge

一个基于 **ESP32-C3** 的荣耀智慧屏 X1 智能家居 BLE 桥接项目。

当前已经实现 Apple HomeKit 原生接入，并可直接使用 iPhone 控制中心里的 **Apple TV Remote**。同时保留原装蓝牙遥控器，通过 ESP32-C3 中继给电视。

由于底层已经完成了荣耀电视的 BLE HID、唤醒、Bond、状态同步和实体遥控器中继，后续接入 **Home Assistant / MQTT / HTTP / ESPHome 风格控制层**也比较容易，核心电视控制逻辑无需重做。

ESP32-C3 在当前实现中同时扮演：

- 对电视：BLE HID Peripheral
- 对原装遥控器：BLE Central
- 对 HomeKit：Wi-Fi HomeKit Accessory

最终结构：

```text
                        ┌─────────────────────────┐
                        │        iPhone           │
                        │ HomeKit / Apple TV Remote│
                        └────────────┬────────────┘
                                     │ Wi-Fi / HomeKit
                                     ▼
┌──────────────────┐        ┌──────────────────────┐        ┌──────────────────┐
│ 原装 HDRC-BV1     │ BLE    │      ESP32-C3        │ BLE HID │ 荣耀智慧屏 X1     │
│ 蓝牙遥控器         ├───────►│ Central + Peripheral │───────►│                  │
└──────────────────┘        └──────────────────────┘        └──────────────────┘
```

## 已实现功能

- iPhone 控制中心 Apple TV Remote 控制荣耀智慧屏
- HomeKit 原生 Television accessory
- 上 / 下 / 左 / 右 / OK
- 返回
- Home
- Menu
- 音量 + / -
- 静音
- HomeKit 开关机
- HomeKit 关机自动执行：
  - Power
  - Right
  - OK
- 电视待机后 BLE 广播唤醒
- HomeKit Active 状态跟随电视 BLE 连接状态
- 保留原装实体遥控器
- 原装遥控器按键实时转发给电视
- ESP32 重启后原装遥控器自动恢复 Bond
- Wi-Fi / HomeKit / BLE Bond 保存在 NVS
- 支持 factory / update 两种固件
- GitHub Actions 自动构建
- 支持普通 ESP32-C3 与 LuatOS CORE ESP32-C3 Native USB 版本

---

# 项目目标

荣耀智慧屏 X1 本身没有原生 Apple HomeKit / Apple TV Remote 支持，也没有现成的 Home Assistant 本地控制集成。

目标不是简单做一个“手机网页遥控器”，而是让它尽量表现得像一个真正的 HomeKit Television：

```text
iPhone Apple TV Remote
        ↓
HomeKit
        ↓
ESP32-C3
        ↓
BLE HID
        ↓
荣耀智慧屏 X1
```

同时又不希望牺牲原装实体遥控器，所以最终方案不是单纯模拟一个遥控器，而是：

```text
原装遥控器
    ↓ BLE
ESP32-C3
    ↓ BLE HID
电视
```

ESP32-C3 同时维护两条 BLE 链路。

---

# 硬件

当前目标平台：

- ESP32-C3
- 4 MB Flash

已测试：

- ESP32-C3 Super Mini
- LuatOS / 合宙 CORE ESP32-C3
- LuatOS CORE ESP32-C3 minimal / Native USB

这些板子统一使用同一套 ESP32-C3 编译配置和同一份固件，不再区分专用 build。

如果使用 Native USB，GPIO18 / GPIO19 会被 USB 占用；本项目不依赖这两个 GPIO。

---

# 软件栈

当前固定版本：

| Component | Version |
| --- | --- |
| Arduino-ESP32 | 3.3.8 |
| ESP-IDF | 5.5.x |
| HomeSpan | 2.1.8 |
| NimBLE-Arduino | 2.5.0 |

GitHub Actions 统一使用：

```text
esp32:esp32:esp32c3
PartitionScheme=min_spiffs
CDCOnBoot=cdc
FlashMode=dio
```

普通 ESP32-C3、ESP32-C3 Super Mini 和 LuatOS CORE ESP32-C3 使用同一套配置。

---

# 原装遥控器配置

原装遥控器广播名称：

```text
HDRC-BV1
```

项目现在把原装遥控器身份集中放在：

```text
relay_bridge/config.h
```

默认内容：

```cpp
#define ORIGINAL_REMOTE_MAC "18:70:3B:76:B8:45"
#define ORIGINAL_REMOTE_NAME "HDRC-BV1"
```

## 为什么需要配置 MAC

HDRC-BV1 某些广播场景下，通过扫描 API 读到的地址可能是：

```text
00:00:00:00:00:00
```

但实际连接、Bond 恢复和 NimBLE Security Store 都需要稳定的 BLE identity address。

因此代码会：

1. 优先根据真实 BLE Public Address 识别遥控器
2. 也允许根据 `HDRC-BV1` 名称识别目标广播
3. 如果扫描结果地址为空，则 fallback 到 `ORIGINAL_REMOTE_MAC`
4. Bond 检测、OUR_SEC / PEER_SEC 查询也统一使用该配置

这样换遥控器时只需要改一处。

---

## 如何获取原装遥控器 BLE MAC

需要的是：

> **原装 HDRC-BV1 遥控器自己的 BLE Public Address**

不是 ESP32 的 Wi-Fi MAC，也不是 ESP32 模拟给电视看的 BLE 地址。

### 方法 1：Android + nRF Connect

这是最方便的方法之一。

1. Android 安装 nRF Connect
2. 打开 Scanner
3. 按一下荣耀原装遥控器按键；如果不容易出现，可长按 **Home + Menu** 让它进入更明显的配对广播状态
4. 在扫描结果中找到：

```text
HDRC-BV1
```

5. 查看该设备的 Address

格式类似：

```text
18:70:3B:76:B8:45
```

如果扫描工具同时显示 Address Type，优先确认它是：

```text
Public
```

然后修改：

```cpp
#define ORIGINAL_REMOTE_MAC "你的遥控器MAC"
```

> iPhone / iOS 通常不会向普通 BLE App 暴露真实设备 MAC，所以不推荐用 iPhone 查这个值。

---

### 方法 2：Linux / Raspberry Pi

有蓝牙适配器的 Linux 可以直接使用：

```bash
bluetoothctl
```

然后：

```text
power on
scan on
```

按一下遥控器按键，或者按 **Home + Menu**。

找到类似：

```text
Device 18:70:3B:76:B8:45 HDRC-BV1
```

前面的地址就是需要填入 `config.h` 的 MAC。

扫描结束：

```text
scan off
quit
```

---

### 方法 3：临时用 ESP32 扫描

如果手边只有 ESP32，也可以临时跑一个 NimBLE Scanner，输出设备名称和地址。

核心逻辑类似：

```cpp
void onResult(const NimBLEAdvertisedDevice *device) {
  if (device->haveName() && device->getName() == "HDRC-BV1") {
    Serial.printf("HDRC-BV1: %s type=%u RSSI=%d\n",
                  device->getAddress().toString().c_str(),
                  device->getAddressType(),
                  device->getRSSI());
  }
}
```

然后：

1. 打开 115200 串口
2. 按遥控器按键或 Home + Menu
3. 记录 `HDRC-BV1` 对应的 Public Address
4. 写入 `relay_bridge/config.h`

注意：本项目之所以保留 MAC fallback，就是因为某些扫描包在当前环境下可能出现 `00:00:00:00:00:00`。如果这一包拿不到地址，可以继续等另一种广播状态，或者使用 Android / Linux 工具确认。

---

## 更换遥控器

如果换了一只新的 HDRC-BV1，只需要修改：

```cpp
// relay_bridge/config.h

#define ORIGINAL_REMOTE_MAC "AA:BB:CC:DD:EE:FF"
#define ORIGINAL_REMOTE_NAME "HDRC-BV1"
```

然后重新编译、刷写固件。

由于新遥控器的安全密钥不同，建议：

1. 修改 MAC 并重新编译
2. 刷 `update.bin` 或 `factory.bin`
3. 新遥控器按 **Home + Menu**
4. 等 ESP32 与新遥控器完成 Bond
5. 重启 ESP32 验证自动恢复

如果 NVS 中仍然保留旧遥控器 Bond，必要时再清理旧 BLE Bond；正常情况下，首次换遥控器时重新配对即可。

---

# BLE HID 按键

原装 HDRC-BV1 的 Report ID 1 是一个 8 字节 Keyboard Report：

```text
[modifier, reserved, key1, key2, key3, key4, key5, key6]
```

抓到的主要按键：

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
| Volume + | 0x80 |
| Volume - | 0x81 |
| Mute | 0x7F |

按键释放：

```text
00 00 00 00 00 00 00 00
```

---

# Voice 按键

原装遥控器 Voice 按键除了发送普通 HID key：

```text
0x75
```

还会使用厂商自定义：

```text
Report ID 0x5A
```

传输语音数据。

当前项目**不转发语音音频数据**。

所以：

- Voice HID trigger 可以识别
- 0x5A 音频流被忽略

---

# 模拟原装遥控器身份

为了让荣耀智慧屏正确识别菜单键、唤醒和 HID 行为，ESP32 对电视侧尽量克隆了 HDRC-BV1 的 BLE 信息。

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

PnP ID：

```text
Vendor ID Source: 1
Vendor ID:        0x7D02
Product ID:       0x0002
Product Version:  0x0003
```

---

# 原装遥控器广播状态

抓到过几种 Manufacturer Data：

```text
02 7D 04 00
02 7D 04 11
02 7D 02 00
```

这些状态在不同连接 / 配对阶段会变化。

实际代码不会把这些状态作为唯一判断条件。

关键判断仍然是：

- 名称 / 固定 Public Address
- advertisement 是否 connectable
- Bond 是否存在
- BLE Security 是否成功恢复

---

# 电视唤醒广播

荣耀智慧屏待机后，普通 HID 广播并不足以稳定唤醒电视。

抓取原装遥控器后发现它在冷唤醒 / Power 场景会发送特殊 Manufacturer Data：

```text
02 7D 03 00 <MAC> 01 01
```

原装遥控器示例：

```text
02 7D 03 00 18 70 3B 76 B8 45 01 01
```

ESP32 发送唤醒广播时不会伪造原遥控器地址，而是填入 **ESP32 自己的 BLE Public Address**：

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

Power 按下时启动大约 8 秒的 transient wake advertisement。

---

# Home Assistant

当前仓库的控制层使用 HomeSpan，把电视暴露成 HomeKit Television。

但真正困难的部分其实已经在 BLE 层解决了：

- 荣耀智慧屏 BLE HID 身份模拟
- Power / 方向键 / OK / Back / Home / Menu / Volume / Mute
- 冷唤醒特殊广播
- 电视 BLE 连接状态检测
- 原装遥控器 BLE Central 中继
- Bond / Security / NVS 持久化
- ESP32 重启后的自动恢复

因此接入 Home Assistant 不需要重新逆向电视协议，只需要增加一层控制接口即可。

比较直接的方案包括：

```text
Home Assistant
      ↓
MQTT / HTTP / ESPHome Native API
      ↓
ESP32-C3
      ↓
现有 sendHonorKey() / wake / state 逻辑
      ↓
荣耀智慧屏 X1
```

推荐后续实现顺序：

1. MQTT command topic
2. MQTT state topic
3. Home Assistant MQTT Device Discovery
4. 将 HomeKit 与 Home Assistant 控制层同时保留

例如可以暴露：

```text
honor_x1/power
honor_x1/key
honor_x1/volume
honor_x1/state
```

这样 Home Assistant 可以直接创建 Remote / Media Player / Button / Switch 等实体。

> 当前仓库尚未实现 Home Assistant 接口；这里只是说明现有 BLE bridge 架构已经具备扩展条件。

---

# HomeKit 映射

HomeKit RemoteKey：

| HomeKit | Honor |
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

Mute：

```text
0x7F
```

---

# HomeKit 开关机

## 开机

如果电视 BLE 当前未连接：

```text
HomeKit Active = ON
        ↓
ESP32 发特殊 Wake ADV
        ↓
电视被唤醒
        ↓
电视重新连接 BLE
        ↓
HomeKit Active 同步为 ON
```

## 关机

荣耀智慧屏按 Power 后并不是立即关机，而是弹出确认菜单。

因此 HomeKit OFF 自动执行：

```text
Power
  ↓
等待约 900 ms
  ↓
Right
  ↓
等待约 250 ms
  ↓
OK
```

这样在 Home App 里只需要点击一次关闭。

---

# HomeKit 状态同步

目前电视实际开关状态没有通过荣耀私有 API 获取。

项目使用：

```text
电视 BLE connected    → HomeKit Active = ON
电视 BLE disconnected → HomeKit Active = OFF
```

因此这里严格来说是：

> BLE Connection State Proxy

正常使用场景下与电视开关状态基本一致。

---

# 原装遥控器 Bond 重启问题

这是整个项目里比较隐蔽的一个坑。

现象：

```text
Home + Menu 重新配对
→ 原装遥控器正常工作

ESP32 重启
→ Bond 地址仍然存在
→ secureConnection() 启动
→ Security 无法完成
→ HCI reason 0x08 / 520
```

一开始看起来很像：

- 信号问题
- supervision timeout
- TV 和遥控器抢 BLE
- reconnect timing
- MTU
- 广播状态

最终通过直接读取 NimBLE：

```text
OUR_SEC
PEER_SEC
```

才定位到真正问题。

## 原因

同一个遥控器重新配对后：

- RAM 中的 LTK / EDIV / RAND 已经更新
- Bond 数量没有变化
- ESP 平台的 NimBLE 持久化路径在这个场景下没有覆盖原来的 NVS security record

于是：

```text
重新配对后
RAM = 新密钥

重启后
NVS = 旧密钥
```

表面上：

```text
remoteBond = 1
```

但实际 Security Material 已经不匹配。

## 当前 workaround

认证成功后比较：

```text
启动时 security record
vs
认证后的 security record
```

如果发现：

- LTK
- EDIV
- RAND
- bond_count
- IRK / CSRK
- security flags

发生变化，则主动刷新该遥控器的：

```text
OUR_SEC
PEER_SEC
```

NVS 记录。

修复后已经验证：

```text
ESP32 多次重启
→ 原装遥控器 04 11 广播
→ 自动连接
→ encrypted=1
→ bonded=1
→ HID Report 订阅成功
```

不再需要每次重启后重新按 Home + Menu。

---

# 首次配对

建议顺序：

1. Flash factory firmware
2. 配置 HomeSpan Wi-Fi
3. 添加到 Apple Home
4. 让电视与 ESP32 配对
5. 原装遥控器按住 **Home + Menu**
6. 等 ESP32 与原装遥控器完成 Bond
7. 重启测试自动恢复

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

---

# 固件

GitHub Actions 只编译一次 `relay_bridge`，生成一套通用 ESP32-C3 固件。

普通 ESP32-C3、ESP32-C3 Super Mini 和 LuatOS CORE ESP32-C3 共用下面这两个文件。

## Factory

```text
honor-x1-relay-bridge.factory.bin
```

完整 merged image。

刷写地址：

```text
0x0
```

适用于：

- 新板
- 首次安装
- 需要彻底重置时

可能需要重新配置：

- Wi-Fi
- HomeKit
- TV Bond
- 原装遥控器 Bond

---

## Update

```text
honor-x1-relay-bridge.update.bin
```

纯应用固件。

刷写地址：

```text
0x10000
```

适用于日常升级。

**不要 erase flash。**

这样可以保留：

- Wi-Fi
- HomeKit pairing
- BLE Bonds
- NVS

当前统一编译参数：

```text
esp32:esp32:esp32c3
PartitionScheme=min_spiffs
CDCOnBoot=cdc
FlashMode=dio
```

---

# 串口

```text
115200 baud
```

长期运行版已经减少了大量开发期 Debug 输出，只保留：

- Wi-Fi 状态
- TV BLE connect / disconnect
- 原装遥控器 connect / disconnect
- Security failure
- NVS security refresh
- wake 状态
- HomeKit 关键操作

---

# 已知限制

- 原装遥控器 MAC 需要在 `relay_bridge/config.h` 中配置
- 更换遥控器后需要更新 `ORIGINAL_REMOTE_MAC` 并重新编译
- Voice Report 0x5A 音频没有转发
- HomeKit Active 使用 BLE connection state 作为电视开关状态代理
- HomeKit RemoteKey 本身没有可靠的长按语义，因此没有模拟长按
- 当前实现针对荣耀智慧屏 X1 / HDRC-BV1 实测，不保证其他荣耀 / 华为电视通用

---

# 项目中涉及的技术点

这个项目实际涉及：

- ESP32-C3
- Arduino / ESP-IDF
- BLE GAP
- BLE GATT
- BLE HID
- BLE Central
- BLE Peripheral
- Simultaneous dual BLE links
- BLE Bond
- LTK / EDIV / RAND
- NimBLE Security Store
- NVS persistence
- BLE advertising reverse engineering
- HID Report Map
- Apple HomeKit
- Home Assistant ready architecture
- HomeSpan
- Wi-Fi
- GitHub Actions CI
- Binary factory/update packaging
- Serial diagnostics
- State machine / reconnect recovery

---

# Disclaimer

这是个人逆向和智能家居实验项目。

项目与 Honor、Huawei、Apple、Espressif 无官方关系。

所有品牌、商标归各自权利人所有。
