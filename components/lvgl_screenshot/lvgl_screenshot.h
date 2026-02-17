#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"

#ifdef USE_ESP_IDF

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

namespace esphome {
namespace lvgl_screenshot {

class LvglScreenshot : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void set_port(uint16_t port) { this->port_ = port; }

 protected:
  uint16_t port_{8080};
  httpd_handle_t server_{nullptr};

  // Semaphore pair for synchronising HTTP handler <-> main loop
  SemaphoreHandle_t capture_requested_{nullptr};
  SemaphoreHandle_t capture_done_{nullptr};

  // Intermediate RGB888 buffer (stb input) — allocated in PSRAM
  uint8_t *rgb_buf_{nullptr};

  // JPEG output buffer — allocated in PSRAM
  uint8_t *jpeg_buf_{nullptr};
  size_t jpeg_capacity_{0};
  size_t jpeg_size_{0};

  // True while a capture is in flight (guards against concurrent requests)
  volatile bool in_progress_{false};

  void start_server_();
  void do_capture_();

  static esp_err_t handle_screenshot_(httpd_req_t *req);
  static void jpeg_write_cb_(void *ctx, void *data, int size);

  static LvglScreenshot *instance_;
};

}  // namespace lvgl_screenshot
}  // namespace esphome

#endif  // USE_ESP_IDF
