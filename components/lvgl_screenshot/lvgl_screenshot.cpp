#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "lvgl_screenshot.h"

#include "esphome/core/log.h"
#include <algorithm>
#include <cstring>

#ifdef USE_ESP_IDF
#include "esp_heap_caps.h"
#endif

#ifdef USE_HOST
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#endif

namespace esphome {
namespace lvgl_screenshot {

static const char *const TAG = "lvgl_screenshot";

LvglScreenshot *LvglScreenshot::instance_ = nullptr;

// Context passed to stb's PNG write callback
struct PngWriteCtx {
  uint8_t *buf;
  size_t capacity;
  size_t size;
};

// ---------------------------------------------------------------------------
// png_write_cb_()  –  stb calls this repeatedly as it produces PNG data
// ---------------------------------------------------------------------------
void LvglScreenshot::png_write_cb_(void *ctx, void *data, int size) {
  auto *c = (PngWriteCtx *) ctx;
  if (size <= 0 || !data)
    return;
  size_t avail = c->capacity - c->size;
  size_t copy = std::min((size_t) size, avail);
  if (copy < (size_t) size) {
    ESP_LOGW(TAG, "PNG buffer full — truncating %d → %u bytes", size, (unsigned) copy);
  }
  memcpy(c->buf + c->size, data, copy);
  c->size += copy;
}

// ---------------------------------------------------------------------------
// do_capture_()  –  convert LVGL RGB565 → RGB888, then encode to PNG
// (shared across both platforms)
// ---------------------------------------------------------------------------
void LvglScreenshot::do_capture_() {
  lv_disp_t *disp = lv_disp_get_default();
  if (!disp || !disp->driver || !disp->driver->draw_buf) {
    ESP_LOGE(TAG, "LVGL framebuffer not available");
    this->png_size_ = 0;
    return;
  }

  uint32_t width = (uint32_t) lv_disp_get_hor_res(disp);
  uint32_t height = (uint32_t) lv_disp_get_ver_res(disp);

  // ------------------------------------------------------------------
  // Force a complete screen redraw so the draw buffer contains a full,
  // up-to-date frame.  Without this, only dirty (changed) regions are
  // rendered — leaving stale data from earlier frames in the buffer.
  // ------------------------------------------------------------------
  lv_disp_draw_buf_t *draw_buf = disp->driver->draw_buf;

  // Remember which buffer LVGL will render into
  void *render_buf = draw_buf->buf_act;

  // Temporarily enable full_refresh so LVGL flushes the entire buffer
  // in one call (prevents buf_act swaps between sub-area flushes)
  uint8_t old_full_refresh = disp->driver->full_refresh;
  disp->driver->full_refresh = 1;

  // Mark every pixel as dirty and force an immediate redraw
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(disp);

  // Restore the original setting
  disp->driver->full_refresh = old_full_refresh;

  // render_buf now holds the complete current frame
  auto *lvgl_buf = (lv_color_t *) render_buf;

  // ------------------------------------------------------------------
  // Convert RGB565 → RGB888 into rgb_buf_ (row-major, top-down)
  // ESPHome builds LVGL with LV_COLOR_16_SWAP=1 on both ESP-IDF and SDL,
  // so the green channel is split across green_h and green_l.
  // ------------------------------------------------------------------
  for (uint32_t y = 0; y < height; y++) {
    uint8_t *row = this->rgb_buf_ + y * width * 3u;
    for (uint32_t x = 0; x < width; x++) {
      lv_color_t c = lvgl_buf[y * width + x];

      uint8_t r5 = c.ch.red;
      uint8_t g6 = (uint8_t) ((c.ch.green_h << 3) | c.ch.green_l);
      uint8_t b5 = c.ch.blue;

      // Scale 5-bit → 8-bit and 6-bit → 8-bit by replicating the MSBs
      row[x * 3 + 0] = (uint8_t) ((r5 << 3) | (r5 >> 2));
      row[x * 3 + 1] = (uint8_t) ((g6 << 2) | (g6 >> 4));
      row[x * 3 + 2] = (uint8_t) ((b5 << 3) | (b5 >> 2));
    }
  }

  // ------------------------------------------------------------------
  // Encode RGB888 → PNG via stb_image_write
  // ------------------------------------------------------------------
  PngWriteCtx ctx = {this->png_buf_, this->png_capacity_, 0};
  int stride = (int) (width * 3u);
  stbi_write_png_to_func(LvglScreenshot::png_write_cb_, &ctx,
                         (int) width, (int) height, 3, this->rgb_buf_, stride);

  this->png_size_ = ctx.size;
  ESP_LOGD(TAG, "Captured %ux%u PNG (%u bytes)", width, height, (unsigned) this->png_size_);
}

// ===========================================================================
// ESP-IDF implementation  (esp_http_server + FreeRTOS semaphores + PSRAM)
// ===========================================================================
#ifdef USE_ESP_IDF

void LvglScreenshot::setup() {
  instance_ = this;

  this->capture_requested_ = xSemaphoreCreateBinary();
  this->capture_done_ = xSemaphoreCreateBinary();

  if (!this->capture_requested_ || !this->capture_done_) {
    ESP_LOGE(TAG, "Failed to create semaphores");
    this->mark_failed();
    return;
  }

  lv_disp_t *disp = lv_disp_get_default();
  if (!disp) {
    ESP_LOGE(TAG, "No LVGL display found");
    this->mark_failed();
    return;
  }

  uint32_t width = (uint32_t) lv_disp_get_hor_res(disp);
  uint32_t height = (uint32_t) lv_disp_get_ver_res(disp);

  size_t rgb_size = width * height * 3u;
  this->rgb_buf_ = (uint8_t *) heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
  if (!this->rgb_buf_) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes for RGB buffer in PSRAM", (unsigned) rgb_size);
    this->mark_failed();
    return;
  }

  this->png_capacity_ = rgb_size;
  this->png_buf_ = (uint8_t *) heap_caps_malloc(this->png_capacity_, MALLOC_CAP_SPIRAM);
  if (!this->png_buf_) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes for PNG buffer in PSRAM", (unsigned) this->png_capacity_);
    this->mark_failed();
    return;
  }

  this->png_size_ = 0;
  this->start_server_();
  ESP_LOGI(TAG, "LVGL screenshot server started — http://<device-ip>:%u/screenshot", this->port_);
}

void LvglScreenshot::start_server_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = this->port_;
  cfg.stack_size = 8192;
  cfg.ctrl_port = (uint16_t) (this->port_ + 1u);

  if (httpd_start(&this->server_, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server on port %u", this->port_);
    this->server_ = nullptr;
    return;
  }

  httpd_uri_t uri = {
      .uri = "/screenshot",
      .method = HTTP_GET,
      .handler = LvglScreenshot::handle_screenshot_,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(this->server_, &uri);
}

void LvglScreenshot::loop() {
  if (xSemaphoreTake(this->capture_requested_, 0) == pdTRUE) {
    this->do_capture_();
    xSemaphoreGive(this->capture_done_);
  }
}

esp_err_t LvglScreenshot::handle_screenshot_(httpd_req_t *req) {
  LvglScreenshot *self = instance_;
  if (!self || !self->png_buf_) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Component not ready");
    return ESP_FAIL;
  }

  if (self->in_progress_) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Capture in progress, try again");
    return ESP_OK;
  }
  self->in_progress_ = true;

  xSemaphoreGive(self->capture_requested_);

  if (xSemaphoreTake(self->capture_done_, pdMS_TO_TICKS(3000)) != pdTRUE) {
    self->in_progress_ = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture timed out");
    return ESP_FAIL;
  }

  if (self->png_size_ == 0) {
    self->in_progress_ = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Framebuffer unavailable");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/png");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screenshot.png\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");

  const size_t CHUNK = 4096;
  size_t sent = 0;
  esp_err_t ret = ESP_OK;
  while (sent < self->png_size_) {
    size_t chunk_len = std::min(CHUNK, self->png_size_ - sent);
    ret = httpd_resp_send_chunk(req, (const char *) self->png_buf_ + sent, (ssize_t) chunk_len);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Failed to send chunk at offset %u", (unsigned) sent);
      break;
    }
    sent += chunk_len;
  }

  httpd_resp_send_chunk(req, nullptr, 0);
  self->in_progress_ = false;
  return ret;
}

#endif  // USE_ESP_IDF

// ===========================================================================
// Host / SDL implementation  (POSIX sockets + pthreads)
// ===========================================================================
#ifdef USE_HOST

void LvglScreenshot::setup() {
  instance_ = this;

  pthread_mutex_init(&this->capture_mutex_, nullptr);
  pthread_cond_init(&this->capture_request_cond_, nullptr);
  pthread_cond_init(&this->capture_done_cond_, nullptr);

  lv_disp_t *disp = lv_disp_get_default();
  if (!disp) {
    ESP_LOGE(TAG, "No LVGL display found");
    this->mark_failed();
    return;
  }

  uint32_t width = (uint32_t) lv_disp_get_hor_res(disp);
  uint32_t height = (uint32_t) lv_disp_get_ver_res(disp);

  size_t rgb_size = width * height * 3u;
  this->rgb_buf_ = (uint8_t *) malloc(rgb_size);
  if (!this->rgb_buf_) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes for RGB buffer", (unsigned) rgb_size);
    this->mark_failed();
    return;
  }

  this->png_capacity_ = rgb_size;
  this->png_buf_ = (uint8_t *) malloc(this->png_capacity_);
  if (!this->png_buf_) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes for PNG buffer", (unsigned) this->png_capacity_);
    this->mark_failed();
    return;
  }

  this->png_size_ = 0;
  this->start_server_();
  ESP_LOGI(TAG, "LVGL screenshot server started — http://localhost:%u/screenshot", this->port_);
}

void LvglScreenshot::start_server_() {
  this->server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (this->server_fd_ < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %s", strerror(errno));
    return;
  }

  int opt = 1;
  setsockopt(this->server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(this->port_);

  if (bind(this->server_fd_, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind on port %u: %s", this->port_, strerror(errno));
    close(this->server_fd_);
    this->server_fd_ = -1;
    return;
  }

  if (listen(this->server_fd_, 2) < 0) {
    ESP_LOGE(TAG, "Failed to listen: %s", strerror(errno));
    close(this->server_fd_);
    this->server_fd_ = -1;
    return;
  }

  this->server_running_ = true;
  pthread_create(&this->server_thread_, nullptr, LvglScreenshot::server_thread_func_, this);
}

void *LvglScreenshot::server_thread_func_(void *arg) {
  auto *self = (LvglScreenshot *) arg;
  while (self->server_running_) {
    int client_fd = accept(self->server_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (self->server_running_)
        ESP_LOGW(TAG, "accept() failed: %s", strerror(errno));
      continue;
    }
    // Set a receive timeout so read() doesn't block forever on
    // connections that never send data (e.g. mDNS probes)
    struct timeval tv = {2, 0};  // 2 second timeout
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    self->handle_client_(client_fd);
    close(client_fd);
  }
  return nullptr;
}

void LvglScreenshot::handle_client_(int client_fd) {
  // Read the HTTP request
  char req_buf[512];
  ssize_t n = read(client_fd, req_buf, sizeof(req_buf) - 1);
  if (n <= 0)
    return;
  req_buf[n] = '\0';

  // Only handle GET /screenshot
  if (strncmp(req_buf, "GET /screenshot", 15) != 0) {
    const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    write(client_fd, resp, strlen(resp));
    return;
  }

  ESP_LOGD(TAG, "Screenshot request received");

  if (this->in_progress_) {
    const char *resp = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: 28\r\n\r\nCapture in progress, retry.\n";
    write(client_fd, resp, strlen(resp));
    return;
  }
  this->in_progress_ = true;

  // Signal the main loop to capture
  pthread_mutex_lock(&this->capture_mutex_);
  this->capture_requested_ = true;
  this->capture_done_ = false;
  pthread_cond_signal(&this->capture_request_cond_);
  pthread_mutex_unlock(&this->capture_mutex_);

  // Wait for the main loop to complete the capture (up to 3 s)
  pthread_mutex_lock(&this->capture_mutex_);
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 3;
  while (!this->capture_done_) {
    if (pthread_cond_timedwait(&this->capture_done_cond_, &this->capture_mutex_, &ts) != 0) {
      ESP_LOGW(TAG, "Capture timed out waiting for main loop");
      break;
    }
  }
  bool done = this->capture_done_.load();
  pthread_mutex_unlock(&this->capture_mutex_);

  if (!done || this->png_size_ == 0) {
    const char *resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
    write(client_fd, resp, strlen(resp));
    this->in_progress_ = false;
    return;
  }

  // Send HTTP response with PNG data
  char header[256];
  int hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: image/png\r\n"
                      "Content-Disposition: inline; filename=\"screenshot.png\"\r\n"
                      "Cache-Control: no-cache, no-store\r\n"
                      "Content-Length: %zu\r\n"
                      "\r\n",
                      this->png_size_);
  write(client_fd, header, hlen);

  size_t sent = 0;
  while (sent < this->png_size_) {
    ssize_t w = write(client_fd, this->png_buf_ + sent, this->png_size_ - sent);
    if (w <= 0)
      break;
    sent += (size_t) w;
  }

  this->in_progress_ = false;
}

void LvglScreenshot::loop() {
  pthread_mutex_lock(&this->capture_mutex_);
  bool requested = this->capture_requested_.load();
  pthread_mutex_unlock(&this->capture_mutex_);

  if (requested) {
    this->do_capture_();

    pthread_mutex_lock(&this->capture_mutex_);
    this->capture_requested_ = false;
    this->capture_done_ = true;
    pthread_cond_signal(&this->capture_done_cond_);
    pthread_mutex_unlock(&this->capture_mutex_);
  }
}

#endif  // USE_HOST

}  // namespace lvgl_screenshot
}  // namespace esphome
