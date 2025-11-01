# RGB Zones API (HeadsetControl)

This document describes the driver-facing API for per-zone RGB control and per-zone brightness. It complements the comments in `src/device.h` and shows small examples for device authors and integrators.

## Capabilities

- `CAP_RGB_ZONES` — indicates the device supports per-zone RGB control (set_zone_rgb / set_all_rgb).
- `CAP_MICROPHONE_MUTE_LED_BRIGHTNESS` — older capability used by many devices for microphone mute LED brightness (0..3). We keep this for compatibility.

Drivers that support per-zone RGB should set the corresponding function pointers in `struct device` and set the capability bit:

- `device->capabilities |= B(CAP_RGB_ZONES);`
- `device->set_zone_rgb = &your_set_zone_rgb_impl;`
- `device->set_all_rgb = &your_set_all_rgb_impl;` (optional)
- `device->set_zone_brightness = &your_set_zone_brightness_impl;` (optional, can be emulated)

Also populate `device->capability_details[CAP_LIGHTS]` or `CAP_RGB_ZONES` as appropriate (usagepage, usageid, interface).

## Implementation notes and examples (driver)

Example: register HS80-like functions in your `init` function:

```c
void mydevice_init(struct device** device) {
    // ... allocate and initialize device struct ...
    device->capabilities |= B(CAP_LIGHTS) | B(CAP_RGB_ZONES);
    device->set_zone_rgb = &mydevice_set_zone_rgb;
    device->set_all_rgb = &mydevice_set_all_rgb; // optional
    device->set_zone_brightness = &mydevice_set_zone_brightness; // optional
}
```

Driver implementation sketch for `set_zone_rgb`:

```c
int mydevice_set_zone_rgb(hid_device* h, uint8_t zone, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t packet[64] = {0};
    // Build packet according to device protocol using zone index and rgb
    // Use hid_write(h, packet, 64) or hid_send_feature_report where appropriate
    return hid_write(h, packet, 64);
}
```

If the device doesn't support native per-zone brightness, you can emulate it by caching current RGB values and re-sending scaled values. The HS80 driver uses this approach: when `set_zone_brightness` is called it scales the cached RGB channels for that zone and calls the existing `set_led_colors` code path.

## CLI and compatibility

- The CLI option `--rgb-mic-brightness` accepts a percentage (0-100).
- For non-HS80 devices that only implement `CAP_MICROPHONE_MUTE_LED_BRIGHTNESS` (numeric 0..3), code maps percent->0..3 using `round(percent * 3 / 100)` to remain compatible.
- For devices that implement `CAP_RGB_ZONES` and `set_zone_brightness`, the full percent value is passed to `set_zone_brightness` so the driver may emulate or apply native brightness.

## Caller example (main.c)

```c
if (device->capabilities & B(CAP_RGB_ZONES)) {
    if (device->set_zone_rgb)
        device->set_zone_rgb(h, 2, 0, 0, 255); // set mic zone to blue
    if (device->set_zone_brightness)
        device->set_zone_brightness(h, 2, 20); // set mic zone to 20%
}

// For older devices supporting only 0..3 mic LED levels:
if (device->capabilities & B(CAP_MICROPHONE_MUTE_LED_BRIGHTNESS)) {
    if (device->send_microphone_mute_led_brightness)
        device->send_microphone_mute_led_brightness(h, mapped_value_0_to_3);
}
```

## Notes

- Keep HID interface and usagepage fields in `device->capability_details[CAP_X]` up-to-date so `dynamic_connect()` opens the correct hid interface.
- Prefer returning non-negative numbers on success and negative codes for errors (the codebase expects `>= 0` for success in many places).

If you want, I can also add a minimal code snippet that demonstrates how to test the driver functions without hardware by stubbing `hid_write` during unit tests.