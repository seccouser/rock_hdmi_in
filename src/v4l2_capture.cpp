#include "v4l2_capture.h"

#include <linux/videodev2.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <poll.h>

static int xioctl(int fd, unsigned long request, void* arg) {
  int r;
  do {
    r = ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

bool V4L2Capture::open_device(const std::string& devnode) {
  fd_ = ::open(devnode.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) return false;

  v4l2_capability cap{};
  if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) == 0) {
    if (debug_) {
      std::fprintf(stderr, "[v4l2_capture] driver=%s card=%s bus=%s caps=0x%x device_caps=0x%x\n",
                   cap.driver, cap.card, cap.bus_info, cap.capabilities, cap.device_caps);
    }
  }

  return true;
}

bool V4L2Capture::configure(uint32_t width, uint32_t height) {
  if (fd_ < 0) return false;

  dmabuf_export_supported_ = false;

  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

  if (xioctl(fd_, VIDIOC_G_FMT, &fmt) < 0) {
    std::fprintf(stderr, "[v4l2_capture] VIDIOC_G_FMT failed: %s\n", std::strerror(errno));
    return false;
  }

  if (width != 0) fmt.fmt.pix_mp.width = width;
  if (height != 0) fmt.fmt.pix_mp.height = height;

  auto try_set_fmt = [&](uint32_t pixfmt, uint32_t planes) -> bool {
    fmt.fmt.pix_mp.pixelformat = pixfmt;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.num_planes = planes;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
      return false;
    }
    return true;
  };

  errno = 0;
  if (!try_set_fmt(V4L2_PIX_FMT_BGR24, 1)) {
    int e = errno;
    std::fprintf(stderr, "[v4l2_capture] VIDIOC_S_FMT BGR3 failed: %s\n", std::strerror(e));
    if (e == EINVAL) {
      errno = 0;
      if (!try_set_fmt(V4L2_PIX_FMT_NV12, 2)) {
        std::fprintf(stderr, "[v4l2_capture] VIDIOC_S_FMT NV12 failed: %s\n", std::strerror(errno));
        return false;
      }
    } else {
      return false;
    }
  }

  v4l2_format got{};
  got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(fd_, VIDIOC_G_FMT, &got) == 0) {
    fmt = got;
  }

  width_ = fmt.fmt.pix_mp.width;
  height_ = fmt.fmt.pix_mp.height;
  fourcc_ = fmt.fmt.pix_mp.pixelformat;

  if ((width != 0 && width_ != width) || (height != 0 && height_ != height)) {
    std::fprintf(stderr,
                 "[v4l2_capture] WARNING: requested %ux%u but driver negotiated %ux%u (HDMI-RX often follows input signal; try setting the source to 1080p or use a scaler/zero-copy path)\n",
                 width, height, width_, height_);
  }
  if (fmt.fmt.pix_mp.num_planes < 1) return false;
  num_planes_ = fmt.fmt.pix_mp.num_planes;

  y_stride_ = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
  uv_stride_ = (num_planes_ >= 2) ? fmt.fmt.pix_mp.plane_fmt[1].bytesperline : 0;

  const uint32_t size0 = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
  const uint32_t size1 = (num_planes_ >= 2) ? fmt.fmt.pix_mp.plane_fmt[1].sizeimage : 0;

  if (fourcc_ == V4L2_PIX_FMT_NV12 && num_planes_ == 1) {
    if (y_stride_ == 0 && height_ != 0) {
      const uint64_t denom = static_cast<uint64_t>(height_) * 3ULL;
      const uint64_t numer = static_cast<uint64_t>(size0) * 2ULL;
      y_stride_ = (denom != 0) ? static_cast<uint32_t>(numer / denom) : width_;
    }
    if (y_stride_ == 0) y_stride_ = width_;
    uv_stride_ = y_stride_;
  } else {
    if (y_stride_ == 0) y_stride_ = width_;
    if (uv_stride_ == 0) uv_stride_ = y_stride_;
  }

  std::fprintf(stderr,
               "[v4l2_capture] negotiated: %ux%u fourcc=0x%08x planes=%u y_stride=%u uv_stride=%u size0=%u size1=%u\n",
               width_, height_, fourcc_, num_planes_, y_stride_, uv_stride_, size0, size1);

  v4l2_requestbuffers req{};
  req.count = reqbuf_count_ < 2 ? 2 : reqbuf_count_;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;

  if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) return false;
  if (req.count < 2) return false;

  buffers_.resize(req.count);

  for (uint32_t i = 0; i < req.count; i++) {
    buffers_[i].dmabuf_fd = -1;
    v4l2_buffer buf{};
    v4l2_plane planes[VIDEO_MAX_PLANES]{};

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    buf.m.planes = planes;
    buf.length = num_planes_;

    if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) return false;

    for (uint32_t p = 0; p < buf.length && p < 2; p++) {
      buffers_[i].planes[p].length = buf.m.planes[p].length;
      buffers_[i].planes[p].start = mmap(nullptr,
                                         buf.m.planes[p].length,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED,
                                         fd_,
                                         buf.m.planes[p].m.mem_offset);
      if (buffers_[i].planes[p].start == MAP_FAILED) return false;
    }

    v4l2_exportbuffer exp{};
    std::memset(&exp, 0, sizeof(exp));
    exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    exp.index = i;
    exp.plane = 0;
    exp.flags = O_CLOEXEC;
    if (xioctl(fd_, VIDIOC_EXPBUF, &exp) == 0) {
      buffers_[i].dmabuf_fd = exp.fd;
      dmabuf_export_supported_ = true;
    }
  }

  if (dmabuf_export_supported_) {
    if (debug_) std::fprintf(stderr, "[v4l2_capture] DMABUF export supported (VIDIOC_EXPBUF ok)\n");
  } else {
    if (debug_) std::fprintf(stderr, "[v4l2_capture] DMABUF export not supported (VIDIOC_EXPBUF failed)\n");
  }

  return true;
}

int V4L2Capture::dmabuf_fd(uint32_t index) const {
  if (index >= buffers_.size()) return -1;
  return buffers_[index].dmabuf_fd;
}

bool V4L2Capture::start() {
  if (fd_ < 0) return false;

  for (uint32_t i = 0; i < buffers_.size(); i++) {
    v4l2_buffer buf{};
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    buf.m.planes = planes;
    buf.length = num_planes_;
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) return false;
  }

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) return false;

  for (int i = 0; i < 12; i++) {
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, 50);
    if (pr <= 0) break;

    v4l2_buffer b{};
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = planes;
    b.length = num_planes_;
    if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
      if (errno == EAGAIN) break;
      break;
    }
    xioctl(fd_, VIDIOC_QBUF, &b);
  }

  return true;
}

bool V4L2Capture::acquire_frame(V4L2Frame& out) {
  if (fd_ < 0) return false;

  out.needs_release = false;
  out.plane0 = nullptr;
  out.plane1 = nullptr;
  out.data.clear();
  out.ts_sec = 0;
  out.ts_usec = 0;

  pollfd pfd{};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  int pr = poll(&pfd, 1, 16);
  if (pr == 0) return true;
  if (pr < 0) return false;

  bool have = false;
  v4l2_buffer last{};
  v4l2_plane last_planes[VIDEO_MAX_PLANES]{};

  while (true) {
    v4l2_buffer buf{};
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = num_planes_;

    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN) break;
      return false;
    }

    if (have) {
      if (xioctl(fd_, VIDIOC_QBUF, &last) < 0) return false;
    }

    last = buf;
    std::memcpy(last_planes, planes, sizeof(last_planes));
    last.m.planes = last_planes;
    have = true;
  }

  if (!have) return true;

  out.width = width_;
  out.height = height_;
  out.fourcc = fourcc_;
  out.num_planes = num_planes_;
  out.y_stride = y_stride_;
  out.uv_stride = uv_stride_;
  out.index = last.index;
  out.needs_release = true;
  out.ts_sec = last.timestamp.tv_sec;
  out.ts_usec = last.timestamp.tv_usec;
  out.plane0 = nullptr;
  out.plane1 = nullptr;
  out.data.clear();

  if (fourcc_ == V4L2_PIX_FMT_NV12) {
    const size_t y_size = static_cast<size_t>(y_stride_) * static_cast<size_t>(height_);
    const size_t uv_size = static_cast<size_t>(uv_stride_) * static_cast<size_t>(height_ / 2);

    if (num_planes_ >= 2) {
      out.plane0 = static_cast<const uint8_t*>(buffers_[last.index].planes[0].start);
      out.plane1 = static_cast<const uint8_t*>(buffers_[last.index].planes[1].start);
    } else if (num_planes_ == 1) {
      const uint8_t* base = static_cast<const uint8_t*>(buffers_[last.index].planes[0].start);
      const size_t avail = buffers_[last.index].planes[0].length;
      if (avail < (y_size + uv_size)) {
        std::fprintf(stderr, "[v4l2_capture] NV12 single-plane buffer too small: have=%zu need=%zu\n", avail, (y_size + uv_size));
        xioctl(fd_, VIDIOC_QBUF, &last);
        return false;
      }
      out.plane0 = base;
      out.plane1 = base + y_size;
    } else {
      std::fprintf(stderr, "[v4l2_capture] NV12 but num_planes_=0\n");
      xioctl(fd_, VIDIOC_QBUF, &last);
      return false;
    }
  } else if (fourcc_ == V4L2_PIX_FMT_BGR24 && num_planes_ >= 1) {
    const uint8_t* src = static_cast<const uint8_t*>(buffers_[last.index].planes[0].start);
    if (!bgr24_to_rgb24(src, width_, height_, out.data)) {
      xioctl(fd_, VIDIOC_QBUF, &last);
      return false;
    }
    out.needs_release = false;
    out.plane0 = nullptr;
    out.plane1 = nullptr;
    if (xioctl(fd_, VIDIOC_QBUF, &last) < 0) return false;
  } else {
    std::fprintf(stderr, "[v4l2_capture] unsupported fourcc=0x%08x planes=%u\n", fourcc_, num_planes_);
    xioctl(fd_, VIDIOC_QBUF, &last);
    return false;
  }

  return true;
}

bool V4L2Capture::release_frame(V4L2Frame& frame) {
  if (!frame.needs_release) return true;
  v4l2_buffer buf{};
  v4l2_plane planes[VIDEO_MAX_PLANES]{};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = frame.index;
  buf.m.planes = planes;
  buf.length = num_planes_;
  if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) return false;
  frame.needs_release = false;
  return true;
}

void V4L2Capture::stop() {
  if (fd_ < 0) return;
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  xioctl(fd_, VIDIOC_STREAMOFF, &type);
}

void V4L2Capture::close_device() {
  for (auto& b : buffers_) {
    if (b.dmabuf_fd >= 0) {
      ::close(b.dmabuf_fd);
      b.dmabuf_fd = -1;
    }
    for (int p = 0; p < 2; p++) {
      if (b.planes[p].start && b.planes[p].start != MAP_FAILED) munmap(b.planes[p].start, b.planes[p].length);
      b.planes[p].start = nullptr;
      b.planes[p].length = 0;
    }
  }
  buffers_.clear();

  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

static inline uint8_t clamp_u8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<uint8_t>(v);
}

bool bgr24_to_rgb24(const uint8_t* bgr, uint32_t width, uint32_t height, std::vector<uint8_t>& rgb_out) {
  if (!bgr || width == 0 || height == 0) return false;
  rgb_out.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);

  size_t in_i = 0;
  size_t out_i = 0;
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);

  for (size_t i = 0; i < pixels; i++) {
    uint8_t b = bgr[in_i + 0];
    uint8_t g = bgr[in_i + 1];
    uint8_t r = bgr[in_i + 2];
    in_i += 3;

    rgb_out[out_i + 0] = r;
    rgb_out[out_i + 1] = g;
    rgb_out[out_i + 2] = b;
    out_i += 3;
  }

  (void)clamp_u8;
  return true;
}

bool nv12_to_rgb24(const uint8_t* y_plane, const uint8_t* uv_plane, uint32_t width, uint32_t height, uint32_t y_stride, uint32_t uv_stride, bool uv_swap, std::vector<uint8_t>& rgb_out) {
  if (!y_plane || !uv_plane || width == 0 || height == 0) return false;
  if (y_stride == 0) y_stride = width;
  if (uv_stride == 0) uv_stride = y_stride;

  rgb_out.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);

  for (uint32_t y = 0; y < height; y++) {
    const uint8_t* yrow = y_plane + static_cast<size_t>(y) * y_stride;
    const uint8_t* uvrow = uv_plane + static_cast<size_t>(y / 2) * uv_stride;

    for (uint32_t x = 0; x < width; x++) {
      int Y = (int)yrow[x] - 16;
      int U = (int)uvrow[(x & ~1) + (uv_swap ? 1 : 0)] - 128;
      int V = (int)uvrow[(x & ~1) + (uv_swap ? 0 : 1)] - 128;
      if (Y < 0) Y = 0;

      int C = 298 * Y;
      int r = (C + 409 * V + 128) >> 8;
      int g = (C - 100 * U - 208 * V + 128) >> 8;
      int b = (C + 516 * U + 128) >> 8;

      size_t oi = (static_cast<size_t>(y) * width + x) * 3;
      rgb_out[oi + 0] = clamp_u8(r);
      rgb_out[oi + 1] = clamp_u8(g);
      rgb_out[oi + 2] = clamp_u8(b);
    }
  }

  return true;
}
