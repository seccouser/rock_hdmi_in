#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct V4L2Frame {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fourcc = 0;
  std::vector<uint8_t> data;

  uint32_t num_planes = 0;
  uint32_t y_stride = 0;
  uint32_t uv_stride = 0;
  const uint8_t* plane0 = nullptr;
  const uint8_t* plane1 = nullptr;
  uint32_t index = 0;
  bool needs_release = false;

  int64_t ts_sec = 0;
  int64_t ts_usec = 0;
};

class V4L2Capture {
public:
  bool open_device(const std::string& devnode);
  bool configure(uint32_t width, uint32_t height);
  bool start();
  bool acquire_frame(V4L2Frame& out);
  bool release_frame(V4L2Frame& frame);
  void stop();
  void close_device();

  void set_nv12_uv_swap(bool swap) { nv12_uv_swap_ = swap; }
  void set_debug(bool dbg) { debug_ = dbg; }
  void set_request_buffer_count(uint32_t n) { reqbuf_count_ = n; }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t fourcc() const { return fourcc_; }
  bool dmabuf_export_supported() const { return dmabuf_export_supported_; }
  int dmabuf_fd(uint32_t index) const;
  uint32_t buffer_count() const { return (uint32_t)buffers_.size(); }

private:
  int fd_ = -1;
  uint32_t buf_type_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t fourcc_ = 0;
  uint32_t bytes_per_frame_ = 0;
  uint32_t num_planes_ = 0;

  uint32_t y_stride_ = 0;
  uint32_t uv_stride_ = 0;

  bool nv12_uv_swap_ = false;

  bool dmabuf_export_supported_ = false;
  bool debug_ = false;
  uint32_t reqbuf_count_ = 4;

  struct Plane {
    void* start = nullptr;
    size_t length = 0;
  };

  struct Buffer {
    Plane planes[2];
    int dmabuf_fd = -1;
  };

  std::vector<Buffer> buffers_;
};

bool bgr24_to_rgb24(const uint8_t* bgr, uint32_t width, uint32_t height, std::vector<uint8_t>& rgb_out);
bool nv12_to_rgb24(const uint8_t* y_plane, const uint8_t* uv_plane, uint32_t width, uint32_t height, uint32_t y_stride, uint32_t uv_stride, bool uv_swap, std::vector<uint8_t>& rgb_out);
bool nv24_to_rgb24(const uint8_t* y_plane, const uint8_t* uv_plane, uint32_t width, uint32_t height, uint32_t y_stride, uint32_t uv_stride, bool uv_swap, std::vector<uint8_t>& rgb_out);
