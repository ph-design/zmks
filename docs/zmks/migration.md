# Migrating from ZMK v0.3 to ZMKs

ZMKs retains ZMK's board, shield, keymap, and module model. Start with a working ZMK v0.3 configuration, change the firmware source, and preserve its existing hardware definitions. Extensions can be enabled separately after the baseline works.

**Scope:** ZMK v0.3 / Zephyr 3.5 configurations. ZMK v0.4 and HWMv2 configurations are not supported yet and must wait for future adaptation. No downgrade script is provided. For a custom branch, identify its actual ZMK and Zephyr versions first; see the [official hardware model upgrade notes](https://zmk.dev/blog/2025/12/09/zephyr-4-1) for v0.4 context.

## Configuration structure

Names below are placeholders. Keep the user's actual board and shield names; neither PH-Lite hardware nor a `ph_lite` target is required.

```text
my-zmk-config/
├── .github/workflows/build.yml
├── build.yaml
├── config/
│   ├── west.yml
│   ├── my_keyboard.keymap
│   └── my_keyboard.conf
├── zephyr/module.yml                 # for a module-style configuration
└── boards/
    ├── arm/my_controller/            # custom v0.3 board, if present
    └── shields/my_keyboard/          # custom shield, if present
```

The manifest selects firmware and dependencies; `build.yaml` selects targets. Board files define the MCU, power, partitions, and bootloader. Shield overlays define keyboard hardware; keymaps define behaviors and logical positions. A module's `zephyr/module.yml` exposes its board root and any custom CMake/Kconfig integration.

Existing projects may instead keep boards under `config/boards`, use external modules, or use upstream definitions. Preserve that discovery mechanism. Do not move directories just to match this example.

## Minimal migration

Keep a working firmware and configuration revision. In the existing `config/west.yml`, select ZMKs while retaining other remotes, projects, and module versions:

```yaml
manifest:
  remotes:
    - name: ph-design
      url-base: https://github.com/ph-design
  projects:
    - name: zmk
      remote: ph-design
      repo-path: zmks
      revision: v0.3-lts
      import: app/west.yml
  self:
    path: config
```

This is a minimal complete example, not a replacement for a manifest containing additional modules. `name: zmk` retains the default checkout path `zmk/app`; `repo-path: zmks` selects the repository. Preserve custom checkout paths and adjust build source paths consistently.

The target [manifest](../../app/west.yml) imports Zephyr `v3.5.0+zmk-fixes` and `ph-design/zmk-studio-messages` revision `v0.3_lts`. Resolve conflicting overrides of these dependencies.

For a workflow currently using the upstream v0.3 reusable build, change the job's `uses` field:

```diff
 jobs:
   build:
-    uses: zmkfirmware/zmk/.github/workflows/build-user-config.yml@v0.3
+    uses: ph-design/zmks/.github/workflows/build-user-config.yml@v0.3-lts
```

Retain other jobs and required inputs. See the [workflow interface](../../.github/workflows/build-user-config.yml). Keep `build.yaml` board/shield names, split roles, snippets, CMake arguments, and artifact names unchanged. A board-only keyboard does not need an added shield.

Build all existing targets using the original workflow or [local build process](https://v0-3-branch.zmk.dev/docs/development/local-toolchain/build-flash), updating west dependencies and using a fresh build directory. Preserve custom module arguments; module-style user configurations require their module root to be discovered, for example through `ZMK_EXTRA_MODULES` in local builds. Inspect generated `zephyr/.config` and `zephyr/zephyr.dts` before flashing.

## PH-Lite source examples

The [PH-Lite configuration repository](https://github.com/ph-design/ph_lite-zmk-config/tree/v0.3-lts) illustrates several configurations sharing one nRF52840 board. Use it to understand file relationships, not as replacement hardware definitions.


| Configuration                                                                                                 | Source organization and relevant differences                                                                                      |
| --------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| [PH60-SC V2](https://github.com/ph-design/ph_lite-zmk-config/tree/v0.3-lts/boards/shields/PH60-SC_V2)         | Matrix/layout in `.dtsi`, LED hardware in `.overlay`, 61-LED mapping and layer colors in `-rgb.dtsi`                              |
| [PH60-SC V2 EVO](https://github.com/ph-design/ph_lite-zmk-config/tree/v0.3-lts/boards/shields/PH60-SC_V2_EVO) | Separate 62-LED mapping; `-lis2dh.dtsi` adds the sensor bus, power, and interrupts                                                |
| [PH60-Multi V2](https://github.com/ph-design/ph_lite-zmk-config/tree/v0.3-lts/boards/shields/PH60-Multi_V2)   | ANSI, ISO, WK, WKL, and HHKB layout files; shared key definitions, matrix transforms, and a complete physical-layout position map |

The shared [board directory](https://github.com/ph-design/ph_lite-zmk-config/tree/v0.3-lts/boards/arm/ph_lite) contains controller defaults and hardware definitions. Its regulator choices, battery sensing, partitions, and pins are specific to that board. Multi-layout positions, Combo positions, and RGB positions must remain consistent with the user's own layouts.

The reference [build matrix](https://github.com/ph-design/ph_lite-zmk-config/blob/v0.3-lts/build.yaml) includes the three release keyboards with locked/unlocked Studio variants and a separate `settings_reset` maintenance image. Do not adopt the `_unsafe` variants or settings erasure as migration defaults. `s60test` is outside this matrix; the dongle entry is commented out. The `config/ph_lite_dongle.*` files describe a receiver role and do not establish a complete active build target or tested receiver release.

## Extension examples

### Studio

Follow the [official v0.3 Studio setup](https://v0-3-branch.zmk.dev/docs/features/studio) for physical layouts, USB transport, and an accessible `&studio_unlock` binding. Keep existing snippets and parameters when adding `studio-rpc-usb-uart` and `-DCONFIG_ZMK_STUDIO=y` to a target. Split keyboards normally enable this USB transport on the central side. Use [ZMKs Studio](https://zmks.phdesign.cc/) with the matching firmware and protocol.

### Hold-tap and Combo definitions

Merge these nodes into an existing keymap that includes `behaviors.dtsi` and the keycode header. The Combo example assumes logical key positions `0` and `1` exist:

```dts
/ {
    behaviors {
        ht_custom: ht_custom {
            compatible = "zmk,behavior-hold-tap";
            #binding-cells = <2>;
            flavor = "tap-preferred";
            tapping-term-ms = <200>;
            bindings = <&kp>, <&kp>;
        };
    };

    combos {
        compatible = "zmk,combos";
        combo_slot_0 {
            reserved;
            key-positions = <0 1>;
            bindings = <&none>;
            timeout-ms = <50>;
        };
    };
};
```

Use `&ht_custom LCTRL A` at one existing key position: tap produces `A`, hold produces `LCTRL`. Keys sharing an instance share its runtime parameters. The current Studio discovers custom presets using an `ht_` display-name prefix and compatible parameter metadata; the example uses that prefix for both node name and label. See the [binding](../../app/dts/bindings/behaviors/zmk,behavior-hold-tap.yaml) and [client discovery code](https://github.com/ph-design/zmks-studio/blob/d4114ec93130a83bbd4a3fbe879aa26716839920/src/behaviors/holdTapUtils.ts).

The reserved Combo starts disabled; assigning valid positions at runtime enables it. Clearing positions disables it. Compiled child nodes determine slot capacity, and their initial position counts contribute to maximum combo length. Positions are logical key indices, not GPIO or HID codes. Preserve existing system combos. See the [binding](../../app/dts/bindings/zmk,combos.yaml) and [implementation](../../app/src/combo.c).

### RGB mapping

Keep the existing LED driver, power control, color order, and physical chain definition. Layer effects require `CONFIG_ZMK_RGB_UNDERGLOW=y` and `CONFIG_EXPERIMENTAL_RGB_LAYER=y`. The mapping direction is **`map[strip_idx] = binding_idx`**; its length equals `chain-length`. Coordinates and layer bindings follow logical key order.

This fragment assumes exactly four logical keys and four LEDs wired in key order `1, 0, 3, 2`, with existing behavior definitions available:

```dts
#include <dt-bindings/zmk/rgb_colors.h>

/ {
    rgb_transform: rgb_transform {
        compatible = "zmk,rgb-matrix-transform";
        map = <1 0 3 2>;
        led-positions = <0 0>, <100 0>, <0 100>, <100 100>;
    };

    underglow-layer {
        compatible = "zmk,underglow-layer";
        transform = <&rgb_transform>;
        base_rgb {
            layer-id = <0>;
            bindings = <&ug ___ &ug ___ &ug ___ &ug ___>;
        };
    };
};
```

`___` means transparent; `BLACK` means explicit black. Recalculate all counts and positions for the target keyboard; PH-Lite's 61/62-LED arrays are not generic. Multi-LED keys or keys without LEDs require checking indexing in the current implementation. See the [transform](../../app/dts/bindings/zmk,rgb-matrix-transform.yaml), [layer binding](../../app/dts/bindings/zmk,underglow-layer.yaml), and [runtime code](../../app/src/rgb_underglow.c).

### Hardware-dependent extensions

Motion requires a compatible LIS2DH device, bus, interrupts, power, and orientation. Enabling `CONFIG_ZMK_MOTION` alone is insufficient; use [Kconfig](../../app/Kconfig), the [sensor binding](../../app/module/dts/bindings/sensor/zmk,lis2dh-common.yaml), and EVO's file structure as references while retaining actual hardware values.

2.4GHz uses separate keyboard (`CONFIG_ZMK_2G4`) and USB receiver (`CONFIG_ZMK_2G4_DONGLE`) roles. The current [ESB driver](../../app/module/drivers/esb/Kconfig) requires nRF52. Both ends need matching protocol and connection configuration; `CONFIG_ZMK_2G4_AES_KEY` requires 32 hexadecimal characters. Use project-specific keys rather than reference values. Baseline migration does not automatically enable either role.

## Validation and special cases

Build each target, then test input, layers, required connection modes, save/reconnect, and sleep/wake on hardware. Record firmware/configuration commits, resolved dependencies, results, and remaining issues. A successful build does not establish hardware compatibility.

Saved Studio settings may override compiled defaults; record personal settings before changing or clearing them. Unknown bindings, Kconfig symbols, and custom C APIs require a source-level adaptation using the [adaptation specification](agent.md). Do not delete features, guess pins, or erase settings to make a migration appear successful.
