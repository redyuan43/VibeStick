# VibeStick 安装与固件升级

本文档说明如何安装 VibeStick、编译固件，以及通过 USB 或 Wi-Fi OTA
升级四个独立设备目标：

| 设备 | board 参数 | USB | OTA | 运动能力 |
| --- | --- | --- | --- | --- |
| M5Stack StickS3 | `sticks3` | 支持 | 支持 | IMU / LIFT |
| M5StickC Plus 1.1 | `stickc_plus` | 支持 | 支持 | IMU / LIFT |
| M5StickC Plus SE | `stickc_plus_se` | 支持 | 支持 | 无 IMU，禁用 LIFT |
| Cardputer Adv | `cardputer_adv` | 支持 | 支持 | 键盘 / Opt / air mouse |

四个目标的固件、版本号、NVS 兼容验证和 OTA channel 不能混用。USB
烧录会写入 bootloader、partition table 和 app；OTA 只发布 app bin，
要求设备已经有 OTA 分区布局。

## 1. 准备环境

进入仓库根目录：

```sh
cd ~/github/VibeStick
```

如果还没有初始化本地配置，先运行：

```sh
./scripts/setup.sh
```

本地密码和 token 的提交规则见：
[本地密钥与 Wi-Fi 配置](LOCAL_SECRETS.zh-CN.md)。

编辑固件网络配置：

```sh
open -e firmware/sticks3/include/vibe_stick_secrets.h
```

本仓库 `.env` 只配置 `8878` 电池遥测服务。ASR、provider、录音、OTA
服务和粘贴行为在 CapsWriter 项目中配置。

`vibe_stick_secrets.h` 至少需要填写：

```c
#define VIBE_STICK_WIFI_SSID "your-2.4g-wifi"
#define VIBE_STICK_WIFI_PASSWORD "your-password"
#define VIBE_STICK_BRIDGE_HOST "192.168.x.x"
#define VIBE_STICK_BRIDGE_PORT 8765
```

如果需要让设备在多个地点自动连接不同 Wi-Fi，可以额外填写多组 profile：

```c
#define VIBE_STICK_WIFI_PROFILES \
    { \
        { VIBE_STICK_WIFI_SSID, VIBE_STICK_WIFI_PASSWORD }, \
        { "your-office-2.4g-wifi", "your-office-password" }, \
    }
```

固件会把这些 profile 合并保存到 ESP 的 NVS 中。普通 OTA 升级和不擦除 flash 的 USB 烧录会保留 NVS，所以设备从一个地点拿到另一个地点时，会在已保存的 Wi-Fi 之间自动重试切换。只有执行 `erase-flash` 或代码主动擦除 NVS 时才会清掉这些配置。

Wi-Fi 必须是 2.4GHz。S3 / ESP32-S3 和 Plus / ESP32 都不能连接 5GHz Wi-Fi。

## 2. 加载 ESP-IDF

如果本机还没有 ESP-IDF，先安装一次：

```sh
if [ ! -d "$HOME/esp/esp-idf" ]; then
  mkdir -p ~/esp && cd ~/esp
  git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && ./install.sh esp32,esp32s3
fi
```

每个新终端在编译或烧录前都要加载 ESP-IDF：

```sh
. "$HOME/esp/esp-idf/export.sh"
```

如果看到 `idf.py was not found on PATH` 或 `command not found: idf.py`，就是当前 shell 还没有执行上面的 `export.sh`。

## 3. 查看 USB 端口

Linux 上常见端口：

| 设备 | 常见端口 |
| --- | --- |
| StickS3 | `/dev/ttyACM0` |
| M5StickC Plus | `/dev/ttyUSB0` |
| M5StickC Plus SE | `/dev/ttyUSB0` |
| Cardputer Adv | `/dev/ttyACM0` |

实际端口以本机枚举为准：

```sh
ls -l /dev/serial/by-id /dev/ttyACM* /dev/ttyUSB*
```

## 4. USB 编译和烧录

USB 是最稳的升级方式，也是在切换到 OTA 分区布局时必须执行的一次完整烧录。

编译 S3：

```sh
./scripts/firmware.sh sticks3 build
```

烧录 S3：

```sh
./scripts/firmware.sh sticks3 -p /dev/ttyACM0 -b 115200 flash monitor
```

编译 Plus：

```sh
./scripts/firmware.sh stickc_plus build
```

烧录 Plus：

```sh
./scripts/firmware.sh stickc_plus -p /dev/ttyUSB0 -b 115200 flash monitor

# Plus SE 与 Plus 1.1 都使用 FTDI 串口；固定使用 115200。
./scripts/firmware.sh stickc_plus_se -p /dev/ttyUSB0 -b 115200 flash monitor

# Cardputer Adv
./scripts/firmware.sh cardputer_adv -p /dev/ttyACM0 -b 115200 flash monitor
```

`scripts/firmware.sh` 会拒绝任何非 `115200` 的 flash 波特率。如果自动
烧录失败，让设备进入下载模式后重试。烧录完成后终端应出现
`Hash of data verified`。

## 5. Wi-Fi OTA 编译和发布

OTA 依赖三个条件：

- 设备已经通过 USB 烧录过带 OTA 分区布局的固件。
- 设备和本机 bridge 在同一个可互通的 2.4GHz Wi-Fi 网络。
- CapsWriter M5 bridge 正在运行并监听 `0.0.0.0:8765`。

Python 的 `scripts/dev.sh` 只启动独立电池遥测服务 `8878`，不能替代
CapsWriter 的语音和 OTA bridge。

发布 S3 OTA：

```sh
./scripts/firmware.sh sticks3 build
./scripts/ota_publish.sh sticks3
```

也可以用统一发布脚本，但必须显式指定受影响目标：

```sh
./scripts/release-firmware.sh sticks3

# 本次公共架构重构影响四板：
./scripts/release-firmware.sh all
```

服务或设备暂时不可用时可先执行
`./scripts/release-firmware.sh --local-only <board...|all>`；这只完成本地
构建、容量检查、OTA 发布和 manifest 校验，不代表最终交付完成。

发布 Plus OTA：

```sh
./scripts/firmware.sh stickc_plus build
./scripts/ota_publish.sh stickc_plus
```

发布 Plus SE 和 Cardputer Adv OTA：

```sh
./scripts/firmware.sh stickc_plus_se build
./scripts/ota_publish.sh stickc_plus_se
./scripts/firmware.sh cardputer_adv build
./scripts/ota_publish.sh cardputer_adv
```

OTA 文件会写入：

```text
firmware/sticks3/ota/<board>.json
firmware/sticks3/ota/<board>.bin
```

CapsWriter bridge 会通过下面两个接口提供 OTA：

```text
/ota/manifest?board=sticks3
/ota/bin?board=sticks3
```

其他目标对应把 `sticks3` 换成 `stickc_plus`、`stickc_plus_se` 或
`cardputer_adv`。设备连上 Wi-Fi 后会自动检查 manifest；只有 manifest
的语义化版本严格高于运行版本才允许下载。相同或更低版本即使 hash、
ELF hash 或 build ID 不同也必须拒绝。

发布后必须从实时 CapsWriter 服务逐板核对：

```sh
curl -fsS "http://192.168.31.225:8765/ota/manifest?board=sticks3"
curl -fsS "http://192.168.31.225:8765/ota/manifest?board=stickc_plus"
curl -fsS "http://192.168.31.225:8765/ota/manifest?board=stickc_plus_se"
curl -fsS "http://192.168.31.225:8765/ota/manifest?board=cardputer_adv"
```

返回的 `version`、`build_id`、`size`、`sha256` 和 `elf_sha256` 必须与
刚生成的本地 manifest 一致。随后逐板确认设备启动日志报告新版本，并
验证 OTA 安装成功以及低版本 manifest 被拒绝。

## 6. 选择 USB 还是 OTA

| 场景 | 推荐方式 |
| --- | --- |
| 第一次烧录设备 | USB |
| 修改了 partition table / bootloader / sdkconfig 分区布局 | USB |
| Wi-Fi 链路不稳定、OTA 很慢或失败 | USB |
| 日常只更新 app 代码，Wi-Fi 稳定 | OTA |
| 同时更新多种板型 | 分别用对应 board 参数构建、发布和验证 |

当前如果 S3 OTA 传输出现高延迟、长时间卡在几十 KB TCP 队列、或者 ping 超过几百毫秒，优先改用 USB 烧录。

## 7. 常见问题

### S3 OTA 很慢或失败

先确认网络质量：

```sh
ping -c 10 <S3-IP>
```

如果 RTT 经常超过几百毫秒，或者出现丢包，OTA 和实时音频上传都会受影响。把设备靠近路由器，换一个干净的 2.4GHz 热点，或者直接用 USB 烧录。

### S3 烧录后没有自动重启

部分 S3 的 USB/JTAG 串口不一定能被 esptool 成功 hard reset。烧录完成后手动按 Reset 键启动 app。

### 四个目标的固件会不会混

不会，只要命令里的 board 参数正确：

```sh
./scripts/firmware.sh sticks3 ...
./scripts/firmware.sh stickc_plus ...
./scripts/firmware.sh stickc_plus_se ...
./scripts/firmware.sh cardputer_adv ...
```

脚本会使用不同的 build 目录、sdkconfig 和 CMake board 定义。
