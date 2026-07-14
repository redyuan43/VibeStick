# VibeStick 本机操作记录

## 固件烧录

- M5StickC Plus 1.1 串口烧录时使用 `115200` 波特率；默认 `460800` 可能在写入前的 flash 校验阶段失败。

## Bridge 端口约定

- `/home/ivan/github/capswriter-agx-client` 的 M5 bridge 使用固定端口 `8765`，作为设备日常使用的默认接收端。
- 本项目 `scripts/dev.sh` 仅用于独立调试 Python bridge，默认使用端口 `8766`，避免与 CapsWriter 冲突。
- 日常连接设备时只启动 CapsWriter；不要同时启动两个 bridge，也不要把 `scripts/dev.sh` 改回 `8765`。
- 固件仍以 CapsWriter 的 `192.168.31.225:8765` 为目标；使用 `scripts/dev.sh` 调试时，需要显式安排匹配的测试目标，不能假定设备会自动连接 `8766`。
