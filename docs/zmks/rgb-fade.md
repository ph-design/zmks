# RGB underglow fades

Set the defaults in your board or shield `.conf`:

```ini
CONFIG_ZMK_RGB_UNDERGLOW_FADE=y
CONFIG_ZMK_RGB_UNDERGLOW_FADE_MS=180
```

The switch defaults to off. The duration is 1–1000 ms in each direction.
The curve starts and ends slowly; reversing a fade continues from its current
level. Manual on/off, idle and USB auto-off use the same transition.
Low battery and System OFF skip the transition.

In Studio, open **Lighting → RGB Underglow → Color** and set **Fade in/out**
and **Fade duration (ms)**. A duration change is sent on Enter or when leaving
the field. The keyboard saves these two values immediately, separately from
its existing RGB settings. A saved value takes precedence over the build
default. Reset Settings removes that override. Older firmware does not show
these controls.

Release the messages revision first, then pin it in the firmware manifest and
the ts-client messages submodule. The local EVO and Studio test builds use
the local schema and client instead of the published revisions.
