# RGB underglow fades

Set the defaults in your board or shield `.conf`:

```ini
CONFIG_ZMK_RGB_UNDERGLOW_FADE=y
CONFIG_ZMK_RGB_UNDERGLOW_FADE_MS=180
```

The switch defaults to off. The duration is 1–2000 ms in each direction.
The curve starts and ends slowly; reversing a fade continues from its current
level. Manual on/off, idle and USB auto-off use the same transition.
Low battery and System OFF skip the transition.

In Studio, open **Lighting → General**. Edits take effect on the keyboard
immediately. Choose **Save to keyboard** to write them to flash, or
**Discard** to restore the saved settings.

- **Fade in/out** sets the switch and duration. A saved value takes precedence
  over the build default.
- **RGB sleep** sets the idle timeout. Use 0 to disable the timer. The USB
  switch applies only to RGB.
- **Light up when a layer is active** lets selected layers show their layer
  colors while the main RGB lighting is off. Set the colors under **Layer
  LED**. The highest selected active layer with a visible color is shown
  immediately. Layer selection follows layer IDs when layers are reordered.
  This control is available on non-split keyboards with layer lighting.

Studio shows the settings supported by the connected firmware.

Release the messages revision first, then pin it in the firmware manifest and
the ts-client messages submodule. The local EVO and Studio test builds use
the local schema and client instead of the published revisions.
