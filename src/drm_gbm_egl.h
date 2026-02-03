#pragma once

#include <cstdint>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <xf86drmMode.h>

struct GbmEglDrm {
  int drm_fd = -1;

  bool debug = false;

  struct gbm_device* gbm_dev = nullptr;
  struct gbm_surface* gbm_surf = nullptr;
  struct gbm_bo* prev_bo = nullptr;
  struct gbm_bo* prev_bo2 = nullptr;
  struct gbm_bo* cur_bo = nullptr;
  bool modeset_done = false;
  bool pageflip_pending = false;

  bool pageflip_enabled = true;
  bool pageflip_use_event = true;

  uint32_t pageflip_timeouts = 0;

  uint64_t pageflip_submitted = 0;
  uint64_t pageflip_completed = 0;
  uint64_t pageflip_dropped = 0;

  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLConfig egl_config = nullptr;
  EGLContext egl_context = EGL_NO_CONTEXT;
  EGLSurface egl_surface = EGL_NO_SURFACE;

  uint32_t gbm_format = 0;

  uint32_t crtc_id = 0;
  uint32_t connector_id = 0;
  uint32_t plane_id = 0;
  uint32_t mode_hdisplay = 0;
  uint32_t mode_vdisplay = 0;

  drmModeModeInfo mode{};

};

bool init_drm_gbm_egl(GbmEglDrm& ctx, const char* drm_node);
bool drm_gbm_egl_make_current(GbmEglDrm& ctx);
bool drm_gbm_egl_swap_buffers(GbmEglDrm& ctx);
void destroy_drm_gbm_egl(GbmEglDrm& ctx);
