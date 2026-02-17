# esphome-lvgl-screenshot

An ESPHome custom component that serves a live JPEG screenshot of the LVGL framebuffer over HTTP.

Designed specifically for **ESP32-P4** with an RGB parallel display running LVGL via ESPHome.
Unlike other screenshot components, this one reads directly from the LVGL draw buffer rather than
the display driver's framebuffer, making it compatible with custom display backends.

## Requirements

- ESP32-P4 (or any ESP-IDF based ESPHome target)
- LVGL configured in ESPHome (`lvgl:` component)
- `buffer_size: 100%` recommended (ensures a complete frame is always in the buffer)
- PSRAM (~1.8 MB total: ~1.1 MB RGB buffer + ~686 KB JPEG output buffer for an 800×480 display)

## Installation

Copy the `components/lvgl_screenshot/` folder into your ESPHome project's `components/` directory,
then reference it in your YAML:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [lvgl_screenshot]

lvgl_screenshot:
  port: 8080  # optional, default 8080
```

## Usage

Once flashed, open a browser and navigate to:

```
http://<device-ip>:8080/screenshot
```

The response is a JPEG image (quality 80) of whatever is currently on screen.

### Home Assistant Generic Camera

Add via **Settings → Integrations → Add Integration → Generic Camera**:

- **Still image URL:** `http://<device-ip>:8080/screenshot`
- **Content type:** `image/jpeg`

## How it works

1. An HTTP GET to `/screenshot` signals the ESPHome main loop via a FreeRTOS binary semaphore.
2. The main loop captures the LVGL draw buffer (`lv_disp_get_default()->driver->draw_buf->buf_act`),
   converts it from RGB565 to RGB888 in PSRAM, then encodes it to JPEG using
   [stb_image_write](https://github.com/nothings/stb) at quality 80.
3. The HTTP handler waits (up to 3 s) for the capture to complete, then streams the JPEG in 4 KB chunks.

All LVGL buffer access happens on the ESPHome main task, keeping it thread-safe.

## Notes

- ESPHome builds LVGL with `LV_COLOR_16_SWAP=1`, so the green channel is reconstructed from
  the `green_h` and `green_l` bitfields.
- Only one screenshot request is served at a time; concurrent requests receive a 503 response.
- The HTTP server runs on its own port (default 8080) and does not depend on ESPHome's `web_server` component.

## Inspired by

[ay129-35MR/esphome-display-screenshot](https://github.com/ay129-35MR/esphome-display-screenshot) —
adapted for LVGL framebuffer access and ESP-IDF native HTTP server.
