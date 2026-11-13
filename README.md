[中文](README.zh.md) | **English**

# ZMKs

ZMKs is a [ZMK Firmware](https://github.com/zmkfirmware/zmk) fork developed by [PH Design](https://github.com/ph-design) for custom keyboard production and product development. It retains ZMK's configuration model and flexibility while extending multi-transport connectivity, lighting, interaction, and visual configuration to support more everyday use cases and product design requirements.

The project follows upstream ZMK and community code and ideas, with firmware and [ZMKs Studio](https://zmks.phdesign.cc/) improvements driven by real keyboard projects.

[ZMKs Studio](https://github.com/ph-design/zmks-studio) · [Roadmap](ROADMAP.md)

## Extensions

| Feature | Extensions in this fork |
| --- | --- |
| 2.4GHz enhancements | Dedicated Nordic ESB keyboard and receiver implementations, USB / BLE / 2.4GHz output selection, and pairing management |
| Studio configuration | Runtime Hold-tap parameter and Combo slot editing, plus lighting, connection, and device management pages |
| RGB matrix enhancements | Position-based animations, reactive effects, typing heatmap, and layer / Caps Lock / connection indicators |
| Motion interaction | IMU-based case-tap actions, carry sleep, and wake settings |
| Device maintenance | Extended device information, diagnostics, power management, and settings reset / recovery behavior |

## Supported keyboards

PH Design maintains configurations for:

| Model | Configuration repository |
| --- | --- |
| PH60-SC V2 | [ph_lite-zmk-config](https://github.com/ph-design/ph_lite-zmk-config) |
| PH60-SC V2 EVO | [ph_lite-zmk-config](https://github.com/ph-design/ph_lite-zmk-config) |
| PH60-Multi V2 | [ph_lite-zmk-config](https://github.com/ph-design/ph_lite-zmk-config) |

Third-party ZMK v0.3 keyboards can follow the [migration guide](docs/zmks/migration.md) while retaining their board, shield, keymap, compatible modules, and hardware definitions. Additional 2.4GHz, lighting, and sensor capabilities depend on the hardware. Migration from ZMK v0.4 / HWMv2 is not supported yet and awaits future adaptation.

## Collaboration

Many ZMKs improvements grow out of discussions and experiments around real keyboard projects. If you are preparing a new tri-mode custom keyboard product or would like to add a feature to an existing design, get in touch to discuss working together.

## Documentation

For builds, flashing, and Studio enablement, use the official [Building and Flashing](https://v0-3-branch.zmk.dev/docs/development/local-toolchain/build-flash) and [ZMK Studio](https://v0-3-branch.zmk.dev/docs/features/studio) documentation. Use [ZMKs Studio](https://zmks.phdesign.cc/) for this fork's configuration extensions.

See the [TODO list](ROADMAP.md) for planned work and [Issues](https://github.com/ph-design/zmks/issues) for bug reports.

## License and credits

Firmware and PH Design modifications use the [MIT License](LICENSE). Inherited website documentation has [separate licensing information](docs/README.md).

Thanks to ZMK, Zephyr, and community contributors for their code and ideas.

## Contact

[dev@phdesign.cc](mailto:dev@phdesign.cc) · [PHDesign official QQ group](https://qm.qq.com/q/4W58s74gZq) · [Discord](https://discord.gg/8UfQXcefPH)

[Website](https://phdesign.cc/) · [GitHub](https://github.com/ph-design) · [Bilibili](https://space.bilibili.com/4172216) · [Taobao](https://shop268559013.taobao.com)
