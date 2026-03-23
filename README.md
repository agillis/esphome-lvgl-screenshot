# esphome-lvgl-screenshot

An ESPHome custom component that serves a live PNG screenshot of the LVGL framebuffer over HTTP.

Works on **ESP32-P4** (and other ESP-IDF based targets) with an RGB parallel display, as well as
the **ESPHome Host platform (SDL)** for desktop testing. Unlike other screenshot components, this
one reads directly from the LVGL draw buffer rather than the display driver's framebuffer, making
it compatible with custom display backends.

## Requirements

- ESPHome with the `lvgl:` component configured
- **ESP-IDF targets**: PSRAM recommended (~1.8 MB total for an 800×480 display)
- **Host/SDL targets**: works out of the box for desktop testing
- `buffer_size: 100%` recommended (ensures a complete frame is always in the buffer)

## Installation

### As a local component

Copy the `components/lvgl_screenshot/` folder into your ESPHome project's `components/` directory:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [lvgl_screenshot]

lvgl_screenshot:
  port: 8080  # optional, default 8080
```

### From GitHub

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/agillis/esphome-lvgl-screenshot
      ref: main
    components: [lvgl_screenshot]

lvgl_screenshot:
  port: 8080  # optional, default 8080
```

## Usage

Once running, open a browser and navigate to:

```
http://<device-ip>:8080/screenshot
```

The response is a **PNG image** of whatever is currently on screen.

## How it works

1. An HTTP GET to `/screenshot` signals the ESPHome main loop (via a FreeRTOS semaphore on ESP-IDF, or a pthread condition variable on Host).
2. The main loop forces a full LVGL screen redraw, captures the draw buffer, converts it from RGB565 to RGB888, and encodes it to PNG using [stb_image_write](https://github.com/nothings/stb).
3. The HTTP handler waits (up to 3 s) for the capture to complete, then streams the PNG back to the client.

All LVGL buffer access happens on the ESPHome main task, keeping it thread-safe.

## Platform support

| Platform | HTTP server | Threading |
|----------|------------|-----------|
| ESP-IDF (ESP32-P4, etc.) | `esp_http_server` | FreeRTOS semaphores |
| Host / SDL | POSIX sockets | pthreads |

## Notes

- ESPHome builds LVGL with `LV_COLOR_16_SWAP=1`, so the green channel is reconstructed from the `green_h` and `green_l` bitfields.
- Only one screenshot request is served at a time; concurrent requests receive a 503 response.
- The HTTP server runs on its own port (default 8080) and does not depend on ESPHome's `web_server` component.

## Inspired by

[ay129-35MR/esphome-display-screenshot](https://github.com/ay129-35MR/esphome-display-screenshot) — adapted for LVGL framebuffer access and ESP-IDF native HTTP server.
