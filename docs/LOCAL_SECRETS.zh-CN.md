# 本地密钥与 Wi-Fi 配置

本文档说明哪些配置只能保存在本机，不能提交到 GitHub。

## 不能提交的文件

以下文件被 `.gitignore` 忽略，里面可以放真实账号、密码、API key、token 和本机 IP：

| 文件 | 用途 |
| --- | --- |
| `.env` | bridge / ASR / provider 的本地 API key 和运行配置 |
| `firmware/sticks3/include/vibe_stick_secrets.h` | ESP 固件用的 Wi-Fi、bridge host、bridge token |

不要用 `git add -f` 强制提交这些文件。

## 可以提交的文件

以下文件可以提交，因为只能放占位示例，不应包含真实凭据：

| 文件 | 用途 |
| --- | --- |
| `firmware/sticks3/include/vibe_stick_secrets.example.h` | 固件 secrets 示例 |
| `README*.md` / `docs/*.md` | 安装与使用说明，只写 placeholder |

## Wi-Fi 的推荐写法

在本机 ignored 文件 `firmware/sticks3/include/vibe_stick_secrets.h` 中写真实 Wi-Fi：

```c
#define VIBE_STICK_WIFI_SSID "your-2.4g-wifi"
#define VIBE_STICK_WIFI_PASSWORD "your-password"
```

需要让设备记住多个地点时，可以写多组 profile：

```c
#define VIBE_STICK_WIFI_PROFILES \
    { \
        { "home-2.4g", "home-password" }, \
        { "office-2.4g", "office-password" }, \
    }
```

固件启动后会把这些 profile 合并保存到 ESP 的 NVS 中。普通 OTA 升级和不擦除 flash 的 USB 烧录会保留 NVS；执行 `erase-flash` 会清掉这些配置。

## 提交前检查

提交前可以运行：

```sh
git status --short
git grep -n -I -E '真实SSID|真实密码|真实API_KEY' -- .
```

也可以直接确认 ignored secrets 文件没有进入 Git 跟踪：

```sh
git check-ignore -v .env firmware/sticks3/include/vibe_stick_secrets.h
git ls-files .env firmware/sticks3/include/vibe_stick_secrets.h
```

第二条命令没有输出，才表示这些本地 secrets 没有被 Git 跟踪。
