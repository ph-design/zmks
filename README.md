# ZMKs

This is a fork based on [ZMK Firmware](https://github.com/zmkfirmware/zmk) (v0.3 branch), maintained by [PH Design](https://github.com/ph-design). It includes additional cool features not yet available in the upstream ZMK project.

> **Upstream:** [zmkfirmware/zmk v0.3-branch](https://github.com/zmkfirmware/zmk/tree/v0.3-branch)

---

## Differences from Official ZMK v0.3

### 2.4GHz Wireless Transport

- Custom [Nordic ESB](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/protocols/esb/index.html) driver with AES-128 encryption
- 2.4GHz dongle/receiver support
- USB / BLE / 2.4GHz multi-transport seamless switching
- BLE & 2.4GHz radio coexistence management

### RGB Underglow (inspired by [PR #2752](https://github.com/zmkfirmware/zmk/pull/2752))

- Rewritten position-based animation system with 20+ effects
- Layer LED indicators & CapsLock underglow
- ZMK Studio RPC for lighting control

### Other

- ZMK Studio changes ,better UI and UX, you can try it out at [https:/zmks.phdesign.cc](https://zmks.phdesign.cc)
- Skip idle on USB power, wired keyboard behavior when wired.
- A better settings reset firmware allow you to directly boot to bootloader
  
    AND MORE TO COME!

### Documentation

We don't have a documentation, try to read the code, or contact me and AMA.
---

## Upstream ZMK

[![Discord](https://img.shields.io/discord/719497620560543766)](https://zmk.dev/community/discord/invite)
[![Build](https://github.com/zmkfirmware/zmk/workflows/Build/badge.svg)](https://github.com/zmkfirmware/zmk/actions)
[![Contributor Covenant](https://img.shields.io/badge/Contributor%20Covenant-v2.0%20adopted-ff69b4.svg)](CODE_OF_CONDUCT.md)

[ZMK Firmware](https://zmk.dev/) is an open source ([MIT](LICENSE)) keyboard firmware built on the [Zephyr™ Project](https://www.zephyrproject.org/) Real Time Operating System (RTOS). ZMK's goal is to provide a modern, wireless, and powerful firmware free of licensing issues.

Check out the website to learn more: https://zmk.dev/.

You can also come join our [ZMK Discord Server](https://zmk.dev/community/discord/invite).

To review features, check out the [feature overview](https://zmk.dev/docs/). ZMK is under active development, and new features are listed with the [enhancement label](https://github.com/zmkfirmware/zmk/issues?q=is%3Aissue+is%3Aopen+label%3Aenhancement) in GitHub. Please feel free to add 👍 to the issue description of any requests to upvote the feature.

## License

This project is a fork of [ZMK Firmware](https://github.com/zmkfirmware/zmk), licensed under the [MIT License](LICENSE). All modifications by PH Design are also released under the same MIT License.

We don't have the time or energy to submit PRs to upstream ZMK. If you'd like to contribute any of our changes back, feel free to use our code directly — no permission needed. 
