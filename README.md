# PongPower

Pong 的电源、ADSP 与音频：PAS1 加载签名 ADSP 固件，GENI I2C 驱动功放，MI2S 输出到双功放，另有电池/充电器与面板背光。

## 适配硬件

- SM8475 APSS：SMEM `80900000/200000`、TCSR mutex3 `1f43000`、IPCC `ed18000` / INTID261、AOSS `c300000`；APSS DRV2 用 active TCS4（GPU 独占 TCS3）
- LPASS ADSP：PAS1、40 MiB 预留内存、SMP2P 与 GLINK
- 音频：GENI I2C `a94000`（`qupv3_se13_i2c_active`，GPIO48/49），TFA9873 双功放地址 `0x34/0x35`，复位 GPIO1/89，WCD938x，tertiary MI2S SD1
- 电池/充电器：按 NothingOSS `qti_battery_charger.c` 的单电池属性编号解码，不写充电限值
- 背光：NT37705 Visionox OLED（1080×2412），面板 DCS `0x51` 写、`0x52` 回读，SM8475 DSI v2.6 @ `0x0ae94000`

## 仓库关系

Pong 专属实现。协议与服务结构参照 UniXNU-sheng 的 Qualcomm 实现（保留原 BSD-2-Clause-Patent 声明），板级参数来自 NothingOSS `android_kernel_msm-5.10_nothing_sm8475` 与 Pong 实机 DT。

## 说明

- `libPongPower.a` 由 `scripts/build-pong-power-native.py` 生成，由主构建 force-load；`ArmDTPlatform` 在 CPU nubs 发布后调用电源入口。
- 背光细节另见 `README.backlight.md`。固件（`adsp.mbn`）不进 Git，用 `refresh_runtime --power-firmware DIR` 或 `pong_adsp-fw=/abs/dir` 提供。

构建、接口与组件目录见 [UniXNU 组件目录](https://unixnu.com/reference/components)，板级输入与编译入口在 [UniXNU-pong](https://github.com/Chengdong-Yu/UniXNU-pong)。

## 已接入的代码

- `PongAdsp.cpp`：手机自身 signed ADSP 固件、PAS1、40 MiB 预留内存、SMP2P、GLINK、ready/fatal 通知和有界等待。
- 实际 SMEM `80900000/200000`、TCSR mutex3 `1f43000`、IPCC `ed18000` / INTID261、AOSS `c300000`。不使用 Sheng 的 SM8550 IPCC、固件或 PAS36 DTB。
- 通过 `gPhysBase/gPhysSize` 检查 DarwinBoot 实际 RAM 所有权；若覆盖 ADSP carveout，停止加载，不覆盖 XNU RAM。
- 电源请求与 WLAN 共用 APSS active TCS4 的 IOLock；GPU 的 active TCS3 保持独立。只保留启动 minima，不在 handover 时降掉共享电源投票。
- `PongBattery.cpp` / `ShengBattmgr.c`：沿用 UniXNU 协议 API，以 NothingOSS `qti_battery_charger.c` 的单电池属性编号解码；不访问 Xiaomi BOARD_GET 扩展、不写充电限值。
- `PongAudio.cpp`：`adsp_apps`、APM speaker/playback graph、TFA9873 配置、独立 SID1801 DMA 和输出环；只有真实初始化完成才发布 `PongAudio`。尚无真机播放成功记录。

## 板级音频约束

Pong 是 TFA98xx 双功放（I2C `a94000`，地址 `34/35`）及 WCD938x，不能加载 Sheng CS35L43 功放配置。实际 `pinctrl-0` 是 qupv3_se13_i2c_active，GPIO48/49、qup13 mux1、pull-up / 2mA；GPIO52/53 不属于这条总线。复位为 GPIO1/89；本机 DT 未设置 `reset-polarity`，Nothing 驱动默认高电平有效，必须 HIGH 10ms 后 LOW 10ms 释放。此前反向序列将功放一直保持在复位状态。PCM 为 tertiary MI2S SD1（本机 DT rx-lines=0x2）；SD0（tx-lines=0x1）属于反馈，不能用于播放输出。只应用本机容器中的易失性 speaker 配置，不运行校准或写工厂 MTP。仍需真机确认 LPASS 时钟、DMA 周期完成及匹配 Darwin24 的 HAL。CPU 调频/系统睡眠回调尚未接入；保留当前固件策略。

## 构建与输入

`scripts/build-pong-power-native.py` 产生 `libPongPower.a`，由主构建脚本 force-load，`ArmDTPlatform` 在 CPU nubs 已发布后调用。电池 metaclass 延后注册，避免 prelink 静态初始化顺序依赖。

`CONFIG_XNU_PONG_POWER`独立编译和启动电源/ADSP/音频。打包器只在该开关启用时添加 `pong_adsp=2`、`pong_audio=2`。HAL由 `refresh_runtime --enable-audio-hal` 或 `refresh_ui --enable-audio-hal` 安装为现代 `.driver`；工厂提供真实接口，实际端点Ready后才枚举设备。内核仅提供control/output两个内存类型和输出start/stop，不创建capture环。本机 `adsp.mbn` 通过 `refresh_runtime --power-firmware DIR` 放入 `/pong/firmware/adsp.mbn`，也可用 `pong_adsp-fw=/absolute/directory` 选择显式目录；固件不放入Git。

RPMh和APPSSMMU lease由独立 `PongPlatformResources` 提供，不再定义在无线对象内。GPU/Wi-Fi/音频共享唯一 `libPongSmmu4K.a`；SID1801使用与其他客户端相同的禁用context编程、回读和TLBSYNC顺序。公开来源及私有输入路径见 `configs/pong-native-sources.toml` 和 `local.toml.example`。详见[源码审计](https://github.com/Chengdong-Yu/UniXNU-pong/blob/main/migration/ipados18/services74-source-audit.md)；HAL加载和服务发布仍不是实际PCM发声证据。

参考：UniXNU 官方 `docs/zh/porting/power.md`；UniXNU-sheng `0e1f6e5` 的 Qualcomm 协议/服务结构（保留原 BSD-2-Clause-Patent 声明）；NothingOSS `android_kernel_msm-5.10_nothing_sm8475` `sm8475/t` 的 PAS 与电池驱动。

构建成功只说明接口完成链接；需下一轮 REC 日志确认 PAS、通道响应及真实电池服务发布，不能据此声称已经出声或硬件已可用。

services45 已实际读出 0x34/0x35 两个功放 revision=0xb73，并完成
SID1801 的 DMA 映射，尚未完成 graph。services46 合并官方 power.md
所引用的 UniXNU-sheng 552b674 的 GLINK 大 intent 保留修复；已占满的
小空闲 intent 不再挤掉 graph 命令申请的大 intent。同时内存映射命令
等待真实 MEM_MAP_DONE/handle，不能把普通 ACK 当成映射完成。HAL 继续
在 staged 目录，未据此宣称系统播放已接通。

## 许可

`LICENSE` 为 BSD-2-Clause-Patent。每个源文件的 SPDX 声明为准。
