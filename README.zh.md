**中文** | [English](README.md)

# ZMKs

ZMKs 是 [PH Design](https://github.com/ph-design) 基于 [ZMK Firmware](https://github.com/zmkfirmware/zmk) 开发的键盘固件分支，面向量产客制化与键盘产品开发。在保留 ZMK 配置体系和可定制性的基础上，扩展多模连接、灯光、交互与可视化配置，让更多日常使用和产品设计中的需求能够直接落地。

项目持续跟进 ZMK 上游与社区的代码和想法，并结合实际键盘项目改进固件及 [ZMKs Studio](https://zmks.phdesign.cc/)

[ZMKs Studio](https://github.com/ph-design/zmks-studio) [Roadmap](ROADMAP.md)

## 主要扩展


| 功能            | 本分支的扩展                                                                |
| ----------------- | ----------------------------------------------------------------------------- |
| 2.4GHz 增强    | 专用 Nordic ESB 键盘端与接收器端，USB / BLE / 2.4GHz 输出切换及配对管理     |
| Studio 参数配置 | 运行时编辑 Hold-tap 参数及 Combo 槽位，扩展灯光、连接和设备管理页面         |
| RGB 矩阵增强    | 基于 LED 位置的灯效、按键响应与打字热力图，以及层、Caps Lock 和连接状态指示 |
| 动作交互        | 支持通过IMU的敲壳动作、携带休眠与唤醒配置                                  |
| 设备维护        | 扩展设备信息、诊断及电源管理，改进设置清除与恢复流程                        |

## 适配键盘

PH Design 目前维护以下键盘的适配配置:


| 型号                  | 配置仓库                                                              |
| ----------------------- | ----------------------------------------------------------------------- |
| PH60-SC V2            | [ph_lite-zmk-config](https://github.com/ph-design/ph_lite-zmk-config) |
| PH60-SC V2 EVO        | [ph_lite-zmk-config](https://github.com/ph-design/ph_lite-zmk-config) |
| PH60-Multi V2         | [ph_lite-zmk-config](https://github.com/ph-design/ph_lite-zmk-config) |

第三方 ZMK v0.3 键盘可按[迁移指南](docs/zmks/migration.md)复用现有 board、shield、键位和兼容模块，保留原有硬件定义；2.4GHz、RGB 和传感器等扩展按硬件条件启用。ZMK v0.4 / HWMv2 暂不支持迁移，等待后续适配。

## 合作

ZMKs 的许多改进都来自实际键盘项目中的交流与尝试。如果你正在筹备一款新的三模客制化产品，或希望为现有设计补上某项功能，欢迎和我们合作交流。

## 文档

固件构建、刷写与 Studio 启用参考官方文档：[构建与刷写](https://v0-3-branch.zmk.dev/docs/development/local-toolchain/build-flash)、[ZMK Studio](https://v0-3-branch.zmk.dev/docs/features/studio)；本分支的扩展配置使用 [ZMKs Studio](https://zmks.phdesign.cc/)。

后续功能见 [TODO](ROADMAP.md)，问题反馈请提交至 [Issues](https://github.com/ph-design/zmks/issues)。

## 许可与致谢

固件及 PH Design 的修改遵循 [MIT License](LICENSE)。继承自上游的网站文档许可见[说明](docs/README.md)。

感谢 ZMK、Zephyr 及社区贡献者提供的代码与想法。

## 联系方式

[dev@phdesign.cc](mailto:dev@phdesign.cc) · [PHDesign 官方 QQ 群](https://qm.qq.com/q/4W58s74gZq) · [Discord](https://discord.gg/8UfQXcefPH)

[官网](https://phdesign.cc/) · [GitHub](https://github.com/ph-design) · [Bilibili](https://space.bilibili.com/4172216) · [淘宝](https://shop268559013.taobao.com)
