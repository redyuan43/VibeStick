# VibeStick

[English README](README.md)

![VibeStick 首页，显示 Codex 和 Claude 状态](assets/brand/home-screen-preview.png)

![VibeStick 语音输入流程，显示 StickS3 录音状态和 Mac HUD](assets/brand/voice-input-preview.png)

VibeStick 把 M5Stack StickS3、M5StickC Plus、M5StickC Plus SE 和
Cardputer Adv 变成桌面 AI agent 小终端：显示状态、5H/7D 用量、提醒音，
并支持录音后自动转写粘贴到 Mac。

VibeStick 面向 M5Stack StickS3 和 M5StickC Plus，不是 M5Stack 官方项目。Codex、Claude 等第三方 agent 名称只用于说明本地兼容工具和集成。

## 电池放电遥测

仓库另外提供 StickS3 和 M5StickC Plus 1.1 的专用电池测试固件。测试固件保持屏幕与 Wi-Fi 负载稳定，通过无线网络向 bridge 上报电池电压；烧录完成并拔掉 USB 线后仍会继续采样。

它们不是完整 VibeStick 移植。Plus 1.1 测试固件不包含 Agent 界面、录音和提醒音。

常用的十分钟冒烟测试：

```sh
./scripts/battery-test.sh smoke --board sticks3 --port auto
./scripts/battery-test.sh smoke --board stickc_plus_11 --port auto
```

只读实时曲线页面：

```text
http://127.0.0.1:8878/telemetry
```

隔离构建、烧录、完整放电、CSV 导出和结果解释见
[电池遥测文档](docs/BATTERY_TELEMETRY.md)。

## 开始前的准备

- [ ] M5 StickS3 或 M5StickC Plus｜一根数据线｜一台电脑（最好是Mac）
- [ ] Wi-Fi（必须是 2.4GHz） 名称｜Wi-Fi密码｜语音识别模型 API Key
-  语音转写API key 推荐 SiliconFlow：<https://cloud.siliconflow.cn/i/7ZCoy9fU>。国内直连、有免费额度、OpenAI 兼容；演示视频用的就是 SiliconFlow。可改用其他 OpenAI 兼容服务的 `base_url` 和模型名称。
- [ ] 已在 CapsWriter 中配置 ASR、provider 状态和粘贴权限。


## 安装

详细安装、USB 烧录、Wi-Fi OTA 和四目标固件说明见：
[docs/INSTALL.zh-CN.md](docs/INSTALL.zh-CN.md)。

本地 Wi-Fi 密码、API key 和 token 的安全规则见：[docs/LOCAL_SECRETS.zh-CN.md](docs/LOCAL_SECRETS.zh-CN.md)。

你可以手动执行，也可以交给 AI 编程 agent，例如 Claude Code 和 Codex。

> 说明：标 👤 的步骤是需要人亲自动手的物理操作，例如插线、长按/短按电源键、在系统设置里授权。AI agent 请按顺序执行 shell 步骤，执行到 👤 步骤时暂停，让用户完成后再继续。

1. 克隆仓库并创建本地配置文件：

```sh
git clone https://github.com/GaryGaryyy/VibeStick.git
cd VibeStick
./scripts/setup.sh
```

2. 填入人类提前准备好的配置：

```sh
open -e firmware/sticks3/include/vibe_stick_secrets.h
```

在 `vibe_stick_secrets.h` 里填写 Wi-Fi 名称、Wi-Fi 密码、Mac bridge host。只要文件里还保留示例占位值，`scripts/setup.sh` 会尝试把 `VIBE_STICK_BRIDGE_HOST` 自动写成检测到的 en0 局域网 IP。需要记住多个地点的 2.4GHz Wi-Fi 时，可以在同一个 ignored 文件里增加 `VIBE_STICK_WIFI_PROFILES`；固件会把多组 profile 保存到 ESP NVS，普通 OTA 升级会保留。

ASR、provider 观察和粘贴行为在 CapsWriter 中配置，不在本仓库的
`.env` 中配置。本仓库 `.env` 只供 `8878` 电池遥测服务使用。

3. 👤 用 USB-C 数据线把 StickS3 插到 Mac。

4. 👤 让 StickS3 进入下载模式：长按侧面电源键，直到蓝灯双闪、屏幕熄灭。这是 ESP32-S3 烧录必需步骤。

5. 如果本机还没有 ESP-IDF，先安装；然后把它加载到当前 shell。这是一次性工具链安装，下载较大（约 1GB），可能需要几分钟。每开一个新终端，在运行 `idf.py` 前都要先执行加载命令：

```sh
if [ ! -d "$HOME/esp/esp-idf" ]; then
  mkdir -p ~/esp && cd ~/esp
  git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && ./install.sh esp32,esp32s3
fi
. "$HOME/esp/esp-idf/export.sh"
```

也可以按 Espressif [官方指南](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/index.html)安装。如果 `install.sh` 失败，请确认已安装 `git`、`python3`、`cmake`，或改按官方指南处理。如果 ESP-IDF 安装在其他位置，请调整路径。

6. 构建并烧录固件。四个固件目标相互独立，命令里的 board 参数必须和
设备对应：

```sh
./scripts/firmware.sh sticks3 build
./scripts/firmware.sh sticks3 -p /dev/ttyACM0 -b 115200 flash monitor
./scripts/firmware.sh stickc_plus build
./scripts/firmware.sh stickc_plus -p /dev/ttyUSB0 -b 115200 flash monitor
```

Plus SE 和 Cardputer Adv 分别使用 `stickc_plus_se`、`cardputer_adv`。
所有板型串口烧录固定为 `115200`。如果不知道端口，运行
`ls -l /dev/serial/by-id /dev/ttyACM* /dev/ttyUSB*`。等到终端出现
`Hash of data verified`。如果自动烧录失败，请让设备进入下载模式后重试。
更完整的 USB/OTA 升级矩阵见 [安装文档](docs/INSTALL.zh-CN.md)。

7. 👤 短按电源键唤醒屏幕。蓝灯应熄灭、屏幕亮起，此时应看到 VibeStick 首页。联网前可能显示离线。

8. 在 `/home/ivan/github/capswriter-agx-client` 启动 CapsWriter M5 bridge，
并确认它监听 `8765`。macOS App、ASR 和粘贴权限按 CapsWriter 项目说明配置。

9. 👤 当 macOS 提示时，为 CapsWriter 应用授予辅助功能权限。粘贴转写
结果需要这个权限。

10. 检查安装状态：

```sh
./scripts/doctor.sh
```

尽量让固件和遥测检查全部 PASS。CapsWriter 的生产 bridge 诊断在
CapsWriter 项目内执行。

11. 👤 打开任意文本框，单击正面蓝键开始说话，再单击一次发送。长按后松开仍保留按住说话模式。两种方式都会使用设备内置麦克风录音，VibeStick 应自动转写并粘贴。

StickS3 和 M5StickC Plus 都支持拿起录音模式：默认 `PTT` 支持正面按键单击开关录音和长按说话。StickS3 长按侧键 3 秒进入设置；侧键单击不执行操作，快速双击才启动桥接搜网。进入设置后，短按侧键依次切换 `MODE`、`SLEEP`、`VERSION` 页面，短按正面键修改当前值，长按正面键 1.5 秒保存。`MODE` 可选 `PTT` 或 `LIFT`，`SLEEP` 可选 1、2、5、10 分钟（默认 5 分钟）。`LIFT` 会用设备平放状态做基线，拿起开始录音，放回桌面并稳定后发送识别。正面键按下或录音期间，机身状态灯会亮；进入深睡前会强制熄灭。

本仓库的 `./scripts/dev.sh` 只启动 `8878` 电池遥测服务，不能替代
CapsWriter 的语音和 OTA bridge。

## Wi-Fi OTA 固件更新

当前固件使用双 OTA app 分区。第一次从旧 single-app 分区升级到双 OTA 分区时，仍然需要通过 USB 烧录一次完整固件；完成这一次后，后续固件可以通过同一 Wi-Fi 下的 bridge 发布。

OTA 也按四个目标独立发布，不能混用。构建并发布 OTA 包：

```sh
. "$HOME/esp/esp-idf/export.sh"
./scripts/firmware.sh sticks3 build
./scripts/ota_publish.sh sticks3
./scripts/firmware.sh stickc_plus build
./scripts/ota_publish.sh stickc_plus
./scripts/firmware.sh stickc_plus_se build
./scripts/ota_publish.sh stickc_plus_se
./scripts/firmware.sh cardputer_adv build
./scripts/ota_publish.sh cardputer_adv
```

四个目标的版本、二进制和 manifest 相互独立。发布后的文件会写入
`firmware/sticks3/ota/`，CapsWriter 会通过 `/ota/manifest?board=...`
和 `/ota/bin?board=...` 提供给设备。设备只接受语义化版本严格高于当前
运行版本的 manifest；仅 hash 或 build ID 不同不能触发降级。
完整升级说明见 [docs/INSTALL.zh-CN.md](docs/INSTALL.zh-CN.md)。

## 常见问题排查

### `command not found: idf.py`

ESP-IDF 没有加载到当前 shell，或者还没有安装。先 source ESP-IDF 的 `export.sh`，再运行 `idf.py`：

```sh
. $HOME/esp/esp-idf/export.sh
```

如果你的 ESP-IDF 在其他位置，请调整路径。每开一个新终端，在使用 `idf.py` 前都要运行一次。

### 烧录报 "Device not configured" 或连不上串口

重新插拔 USB-C 数据线。再次进入下载模式：长按侧面电源键，直到蓝灯
双闪、屏幕熄灭。运行 `ls /dev/cu.*` 找端口，然后重试
`./scripts/firmware.sh <board> -p <port> -b 115200 build flash`。

### StickS3 连不上 Wi-Fi

请使用 2.4GHz Wi-Fi。StickS3 / ESP32-S3 不支持 5GHz Wi-Fi。

### 录音能转写但没有粘贴

检查 CapsWriter 的辅助功能权限和粘贴诊断。本仓库的 Python 遥测进程
不会执行粘贴。

### "No transcription adapter configured"

在 CapsWriter 中配置 ASR。本仓库没有转写 adapter。

### 找不到 `.env`

`.env` 是仅供本地遥测脚本使用的隐藏文件。用下面命令打开：

```sh
open -e .env
```

### 录音转写失败、SSL 报错或超时

使用 CapsWriter 的 ASR 诊断和配置。本固件只向 `8765` 的 CapsWriter
上传 PCM。

## 配置归属

不要把真实 API key、本地 token、Wi-Fi 密码、本地日志、录音文件提交到 git。

- `firmware/sticks3/include/vibe_stick_secrets.h`：设备 Wi-Fi、
  `192.168.31.225:8765` CapsWriter 地址、bridge profiles 和设备 token。
- `.env`：本地电池遥测的 `VIBE_STICK_TELEMETRY_TOKEN`、
  `VIBE_STICK_TELEMETRY_PORT` 和 `VIBE_STICK_TELEMETRY_URL`。
- CapsWriter 配置：ASR、provider 状态、quota、设备注册、录音、OTA
  服务和粘贴行为。

## 项目结构

```text
VibeStick/
  README.md
  README.zh-CN.md
  .env.example
  docs/
  firmware/sticks3/
  firmware/telemetry/
  bridge/src/vibe_stick/
  app/macos/VibeStickHUD/
  scripts/
  tests/
```

## 检查命令

```sh
./scripts/check.sh
```

固件构建仍需要 ESP-IDF：

```sh
. $HOME/esp/esp-idf/export.sh
./scripts/check.sh --firmware
```

## 当前限制

- 真机回归、串口烧录和实时 OTA 校验需要连接硬件并启动 CapsWriter。
- Cardputer 的 OTA 余量最小，后续增加功能时需要持续关注。

## 贡献与安全

欢迎贡献,详见 [CONTRIBUTING.md](CONTRIBUTING.md)。报告安全漏洞请见
[SECURITY.md](SECURITY.md)(请私下报告)。

## 许可证

VibeStick 使用 MIT License 发布。见 [LICENSE](LICENSE)。
