#include "drm_gbm_egl.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/select.h>
#include <sys/time.h>
#include <cctype>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static drmModeConnector* find_connected_connector(int fd, drmModeRes* res, bool debug) {
  for (int i = 0; i < res->count_connectors; i++) {
    if (debug) {
      std::fprintf(stderr, "[drm_gbm_egl] probing connector %d/%d (id=%u)\n", i + 1, res->count_connectors, res->connectors[i]);
      std::fflush(stderr);
    }
    drmModeConnector* conn = drmModeGetConnectorCurrent(fd, res->connectors[i]);
    if (!conn) conn = drmModeGetConnector(fd, res->connectors[i]);
    if (!conn) continue;
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) return conn;
    drmModeFreeConnector(conn);
  }
  return nullptr;
}

static bool choose_mode_override(drmModeConnector* conn, const char* mode_override, drmModeModeInfo& out) {
  if (!conn || conn->count_modes <= 0) return false;
  if (!mode_override || !mode_override[0]) return false;

  std::string s(mode_override);
  // trim
  size_t b = 0;
  while (b < s.size() && std::isspace((unsigned char)s[b])) b++;
  size_t e = s.size();
  while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
  s = s.substr(b, e - b);
  if (s.empty()) return false;

  auto vrefresh = [](const drmModeModeInfo& m) -> int {
    if (m.vrefresh) return (int)m.vrefresh;
    if (m.htotal && m.vtotal) return (int)((m.clock * 1000) / (m.htotal * m.vtotal));
    return 0;
  };

  // Format: WxH or WxH@R
  int req_w = -1;
  int req_h = -1;
  int req_r = -1;
  {
    size_t x = s.find('x');
    if (x != std::string::npos && x > 0) {
      size_t at = s.find('@', x + 1);
      const std::string sw = s.substr(0, x);
      const std::string sh = (at == std::string::npos) ? s.substr(x + 1) : s.substr(x + 1, at - (x + 1));
      bool ok = !sw.empty() && !sh.empty();
      for (char c : sw) ok = ok && std::isdigit((unsigned char)c);
      for (char c : sh) ok = ok && std::isdigit((unsigned char)c);
      if (ok) {
        req_w = std::atoi(sw.c_str());
        req_h = std::atoi(sh.c_str());
        if (at != std::string::npos) {
          const std::string sr = s.substr(at + 1);
          bool okr = !sr.empty();
          for (char c : sr) okr = okr && std::isdigit((unsigned char)c);
          if (okr) req_r = std::atoi(sr.c_str());
        }
      }
    }
  }

  // If it isn't WxH[[@]Hz], treat it as a mode name (e.g. "3840x2160").
  if (req_w < 0 || req_h < 0) {
    for (int i = 0; i < conn->count_modes; i++) {
      const drmModeModeInfo& m = conn->modes[i];
      if (s == m.name) {
        out = m;
        return true;
      }
    }
    return false;
  }

  int best = -1;
  int best_vr = 0;
  for (int i = 0; i < conn->count_modes; i++) {
    const drmModeModeInfo& m = conn->modes[i];
    if ((int)m.hdisplay != req_w || (int)m.vdisplay != req_h) continue;
    const int vr = vrefresh(m);
    if (req_r >= 0) {
      if (vr != req_r) continue;
      out = m;
      return true;
    }
    if (best < 0 || vr > best_vr) {
      best = i;
      best_vr = vr;
    }
  }
  if (best >= 0) {
    out = conn->modes[best];
    return true;
  }
  return false;
}

static drmModeEncoder* find_encoder(int fd, drmModeRes* res, drmModeConnector* conn) {
  if (conn->encoder_id) {
    drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id);
    if (enc) return enc;
  }
  for (int i = 0; i < conn->count_encoders; i++) {
    drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[i]);
    if (enc) return enc;
  }
  (void)res;
  return nullptr;
}

static bool choose_mode(drmModeConnector* conn, drmModeModeInfo& out) {
  if (!conn || conn->count_modes <= 0) return false;

  auto vrefresh = [](const drmModeModeInfo& m) -> int {
    if (m.vrefresh) return (int)m.vrefresh;
    if (m.htotal && m.vtotal) return (int)((m.clock * 1000) / (m.htotal * m.vtotal));
    return 0;
  };

  auto better = [&](const drmModeModeInfo& cand, const drmModeModeInfo& best) -> bool {
    const uint64_t cp = (uint64_t)cand.hdisplay * (uint64_t)cand.vdisplay;
    const uint64_t bp = (uint64_t)best.hdisplay * (uint64_t)best.vdisplay;
    if (cp != bp) return cp > bp;
    if (cand.hdisplay != best.hdisplay) return cand.hdisplay > best.hdisplay;
    if (cand.vdisplay != best.vdisplay) return cand.vdisplay > best.vdisplay;
    const int cvr = vrefresh(cand);
    const int bvr = vrefresh(best);
    return cvr > bvr;
  };

  // First: prefer connector "preferred" modes.
  int preferred_best = -1;
  for (int i = 0; i < conn->count_modes; i++) {
    const drmModeModeInfo& m = conn->modes[i];
    if ((m.type & DRM_MODE_TYPE_PREFERRED) == 0) continue;
    if (preferred_best < 0 || better(m, conn->modes[preferred_best])) preferred_best = i;
  }
  if (preferred_best >= 0) {
    out = conn->modes[preferred_best];
    return true;
  }

  int best = 0;
  uint64_t best_pixels = (uint64_t)conn->modes[0].hdisplay * (uint64_t)conn->modes[0].vdisplay;
  int best_vr = vrefresh(conn->modes[0]);

  for (int i = 1; i < conn->count_modes; i++) {
    const drmModeModeInfo& m = conn->modes[i];
    uint64_t pixels = (uint64_t)m.hdisplay * (uint64_t)m.vdisplay;
    int vr = vrefresh(m);

    if (pixels > best_pixels) {
      best = i;
      best_pixels = pixels;
      best_vr = vr;
      continue;
    }
    if (pixels < best_pixels) continue;

    if (m.hdisplay > conn->modes[best].hdisplay) {
      best = i;
      best_vr = vr;
      continue;
    }
    if (m.hdisplay < conn->modes[best].hdisplay) continue;

    if (m.vdisplay > conn->modes[best].vdisplay) {
      best = i;
      best_vr = vr;
      continue;
    }
    if (m.vdisplay < conn->modes[best].vdisplay) continue;

    if (vr > best_vr) {
      best = i;
      best_vr = vr;
      continue;
    }
  }

  out = conn->modes[best];
  return true;
}

static bool init_egl_display_and_context(GbmEglDrm& ctx) {
  ctx.egl_display = eglGetDisplay((EGLNativeDisplayType)ctx.gbm_dev);
  if (ctx.egl_display == EGL_NO_DISPLAY) {
    std::fprintf(stderr, "[drm_gbm_egl] eglGetDisplay failed\n");
    return false;
  }

  if (!eglInitialize(ctx.egl_display, nullptr, nullptr)) {
    std::fprintf(stderr, "[drm_gbm_egl] eglInitialize failed (eglGetError=0x%x)\n", (unsigned)eglGetError());
    return false;
  }

  const EGLint cfg_attribs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 0,
      EGL_NONE};

  EGLint num_cfg = 0;
  if (!eglGetConfigs(ctx.egl_display, nullptr, 0, &num_cfg) || num_cfg <= 0) {
    std::fprintf(stderr, "[drm_gbm_egl] eglGetConfigs failed (eglGetError=0x%x)\n", (unsigned)eglGetError());
    return false;
  }

  std::vector<EGLConfig> cfgs((size_t)num_cfg);
  EGLint out_cfgs = 0;
  if (!eglChooseConfig(ctx.egl_display, cfg_attribs, cfgs.data(), num_cfg, &out_cfgs) || out_cfgs <= 0) {
    std::fprintf(stderr, "[drm_gbm_egl] eglChooseConfig failed (eglGetError=0x%x)\n", (unsigned)eglGetError());
    return false;
  }

  auto pick_by_visual = [&](uint32_t desired) -> bool {
    for (int i = 0; i < out_cfgs; i++) {
      EGLint vid = 0;
      if (eglGetConfigAttrib(ctx.egl_display, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid) && (uint32_t)vid == desired) {
        ctx.egl_config = cfgs[i];
        ctx.gbm_format = desired;
        return true;
      }
    }
    return false;
  };

  if (!pick_by_visual(DRM_FORMAT_XRGB8888) &&
      !pick_by_visual(DRM_FORMAT_ARGB8888) &&
      !pick_by_visual(DRM_FORMAT_XBGR8888) &&
      !pick_by_visual(DRM_FORMAT_ABGR8888)) {
    ctx.egl_config = cfgs[0];
    EGLint vid = 0;
    if (eglGetConfigAttrib(ctx.egl_display, ctx.egl_config, EGL_NATIVE_VISUAL_ID, &vid)) {
      ctx.gbm_format = (uint32_t)vid;
    }
    if (ctx.gbm_format == 0) ctx.gbm_format = DRM_FORMAT_XRGB8888;
  }

  std::fprintf(stderr, "[drm_gbm_egl] using GBM/DRM format 0x%x\n", (unsigned)ctx.gbm_format);

  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  ctx.egl_context = eglCreateContext(ctx.egl_display, ctx.egl_config, EGL_NO_CONTEXT, ctx_attribs);
  if (ctx.egl_context == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "[drm_gbm_egl] eglCreateContext failed (eglGetError=0x%x)\n", (unsigned)eglGetError());
    return false;
  }

  return true;
}

static bool create_gbm_and_egl_surface(GbmEglDrm& ctx) {
  ctx.gbm_surf = gbm_surface_create(ctx.gbm_dev,
                                    ctx.mode_hdisplay,
                                    ctx.mode_vdisplay,
                                    ctx.gbm_format,
                                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!ctx.gbm_surf) {
    std::fprintf(stderr, "[drm_gbm_egl] gbm_surface_create failed\n");
    return false;
  }

  ctx.egl_surface = eglCreateWindowSurface(ctx.egl_display, ctx.egl_config,
                                           (EGLNativeWindowType)ctx.gbm_surf, nullptr);
  if (ctx.egl_surface == EGL_NO_SURFACE) {
    std::fprintf(stderr, "[drm_gbm_egl] eglCreateWindowSurface failed (eglGetError=0x%x)\n", (unsigned)eglGetError());
    return false;
  }

  return true;
}

bool init_drm_gbm_egl(GbmEglDrm& ctx, const char* drm_node, const char* mode_override) {
  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] open drm node %s\n", drm_node);
    std::fflush(stderr);
  }
  ctx.drm_fd = open(drm_node, O_RDWR | O_CLOEXEC);
  if (ctx.drm_fd < 0) {
    std::fprintf(stderr, "[drm_gbm_egl] open(%s) failed: %s\n", drm_node, std::strerror(errno));
    return false;
  }

  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] drmModeGetResources...\n");
    std::fflush(stderr);
  }
  drmModeRes* res = drmModeGetResources(ctx.drm_fd);
  if (!res) {
    std::fprintf(stderr, "[drm_gbm_egl] drmModeGetResources failed\n");
    return false;
  }
  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] resources: connectors=%d crtcs=%d encoders=%d\n", res->count_connectors, res->count_crtcs, res->count_encoders);
    std::fflush(stderr);
  }

  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] find connected connector...\n");
    std::fflush(stderr);
  }
  drmModeConnector* conn = find_connected_connector(ctx.drm_fd, res, ctx.debug);
  if (!conn) {
    std::fprintf(stderr, "[drm_gbm_egl] no connected connector found\n");
    drmModeFreeResources(res);
    return false;
  }
  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] selected connector id=%u modes=%d\n", conn->connector_id, conn->count_modes);
    std::fflush(stderr);
  }

  drmModeModeInfo mode{};
  if (mode_override && mode_override[0]) {
    if (ctx.debug) {
      std::fprintf(stderr, "[drm_gbm_egl] mode override requested: %s\n", mode_override);
      std::fflush(stderr);
    }
    if (!choose_mode_override(conn, mode_override, mode)) {
      std::fprintf(stderr, "[drm_gbm_egl] mode override not found: %s\n", mode_override);
      drmModeFreeConnector(conn);
      drmModeFreeResources(res);
      return false;
    }
  } else if (!choose_mode(conn, mode)) {
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return false;
  }
  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] selected mode %s %dx%d@%d\n", mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh);
    std::fflush(stderr);
  }

  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] find encoder...\n");
    std::fflush(stderr);
  }
  drmModeEncoder* enc = find_encoder(ctx.drm_fd, res, conn);
  if (!enc) {
    std::fprintf(stderr, "[drm_gbm_egl] no encoder found\n");
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return false;
  }
  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] encoder id=%u crtc_id=%u\n", enc->encoder_id, enc->crtc_id);
    std::fflush(stderr);
  }

  ctx.connector_id = conn->connector_id;
  ctx.crtc_id = enc->crtc_id;
  ctx.mode_hdisplay = mode.hdisplay;
  ctx.mode_vdisplay = mode.vdisplay;
  ctx.mode = mode;

  drmModeFreeEncoder(enc);
  drmModeFreeConnector(conn);
  drmModeFreeResources(res);

  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] gbm_create_device...\n");
    std::fflush(stderr);
  }
  ctx.gbm_dev = gbm_create_device(ctx.drm_fd);
  if (!ctx.gbm_dev) {
    std::fprintf(stderr, "[drm_gbm_egl] gbm_create_device failed\n");
    return false;
  }

  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] init EGL display/context...\n");
    std::fflush(stderr);
  }
  if (!init_egl_display_and_context(ctx)) return false;
  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] create GBM/EGL surface %ux%u...\n", ctx.mode_hdisplay, ctx.mode_vdisplay);
    std::fflush(stderr);
  }
  if (!create_gbm_and_egl_surface(ctx)) return false;

  if (ctx.debug) {
    std::fprintf(stderr, "[drm_gbm_egl] init done\n");
    std::fflush(stderr);
  }
  return true;
}

bool drm_gbm_egl_make_current(GbmEglDrm& ctx) {
  if (!eglMakeCurrent(ctx.egl_display, ctx.egl_surface, ctx.egl_surface, ctx.egl_context)) return false;
  if (!eglSwapInterval(ctx.egl_display, 0)) {
    std::fprintf(stderr, "[drm_gbm_egl] eglSwapInterval(0) failed (eglGetError=0x%x)\n", (unsigned)eglGetError());
  }
  return true;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void* data) {
  (void)fd;
  (void)frame;
  (void)sec;
  (void)usec;
  auto* ctx = static_cast<GbmEglDrm*>(data);
  if (!ctx) return;
  ctx->pageflip_pending = false;
  ctx->pageflip_completed++;

  if (ctx->gbm_surf && ctx->prev_bo) {
    gbm_surface_release_buffer(ctx->gbm_surf, ctx->prev_bo);
    ctx->prev_bo = nullptr;
  }
  ctx->prev_bo = ctx->cur_bo;
  ctx->cur_bo = nullptr;
}

static void drain_drm_events(GbmEglDrm& ctx) {
  drmEventContext ev{};
  std::memset(&ev, 0, sizeof(ev));
  ev.version = DRM_EVENT_CONTEXT_VERSION;
  ev.page_flip_handler = page_flip_handler;

  while (ctx.pageflip_pending) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(ctx.drm_fd, &fds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int r = select(ctx.drm_fd + 1, &fds, nullptr, nullptr, &tv);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) break;
    drmHandleEvent(ctx.drm_fd, &ev);
  }
}

static void wait_pageflip(GbmEglDrm& ctx, int timeout_ms) {
  if (!ctx.pageflip_pending) return;

  drmEventContext ev{};
  std::memset(&ev, 0, sizeof(ev));
  ev.version = DRM_EVENT_CONTEXT_VERSION;
  ev.page_flip_handler = page_flip_handler;

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(ctx.drm_fd, &fds);

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  int remaining = timeout_ms;
  while (ctx.pageflip_pending && remaining > 0) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx.drm_fd, &rfds);

    timeval rtv{};
    rtv.tv_sec = remaining / 1000;
    rtv.tv_usec = (remaining % 1000) * 1000;

    int r = select(ctx.drm_fd + 1, &rfds, nullptr, nullptr, &rtv);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) break;
    drmHandleEvent(ctx.drm_fd, &ev);
    break;
  }

  if (ctx.pageflip_pending) {
    ctx.pageflip_timeouts++;
    if (ctx.pageflip_timeouts > 10) {
      if (ctx.debug) {
        std::fprintf(stderr, "[drm_gbm_egl] pageflip stuck, resetting (timeouts=%u)\n", ctx.pageflip_timeouts);
      }
      ctx.pageflip_pending = false;
      ctx.pageflip_timeouts = 0;
      ctx.pageflip_enabled = false;
      if (ctx.gbm_surf && ctx.prev_bo) {
        gbm_surface_release_buffer(ctx.gbm_surf, ctx.prev_bo);
        ctx.prev_bo = nullptr;
      }
    }
  } else {
    ctx.pageflip_timeouts = 0;
  }
}

static bool drm_set_crtc_for_bo(GbmEglDrm& ctx, gbm_bo* bo, uint32_t& fb_id_inout) {
  struct FbData {
    int drm_fd;
    uint32_t fb_id;
  };

  auto destroy_fb = [](gbm_bo* b, void* data) {
    FbData* fb = static_cast<FbData*>(data);
    if (fb) {
      if (fb->fb_id) drmModeRmFB(fb->drm_fd, fb->fb_id);
      delete fb;
    }
    (void)b;
  };

  FbData* fb = static_cast<FbData*>(gbm_bo_get_user_data(bo));
  if (!fb) {
    uint32_t handles[4] = {};
    uint32_t strides[4] = {};
    uint32_t offsets[4] = {};

    handles[0] = gbm_bo_get_handle(bo).u32;
    strides[0] = gbm_bo_get_stride(bo);
    offsets[0] = 0;

    const uint32_t width = gbm_bo_get_width(bo);
    const uint32_t height = gbm_bo_get_height(bo);
    const uint32_t format = gbm_bo_get_format(bo);

    uint32_t fb_id = 0;
    int ret = drmModeAddFB2(ctx.drm_fd, width, height, format, handles, strides, offsets, &fb_id, 0);
    if (ret) {
      std::fprintf(stderr, "[drm_gbm_egl] drmModeAddFB2 failed (format=0x%x): %s\n", format, std::strerror(errno));
      return false;
    }

    fb = new FbData{ctx.drm_fd, fb_id};
    gbm_bo_set_user_data(bo, fb, destroy_fb);
  }

  fb_id_inout = fb->fb_id;

  if (!ctx.modeset_done) {
    int set_ret = drmModeSetCrtc(ctx.drm_fd, ctx.crtc_id, fb_id_inout, 0, 0, &ctx.connector_id, 1, &ctx.mode);
    if (set_ret) {
      std::fprintf(stderr, "[drm_gbm_egl] drmModeSetCrtc failed: %s\n", std::strerror(errno));
      return false;
    }
    ctx.modeset_done = true;
  }

  return true;
}

bool drm_gbm_egl_swap_buffers(GbmEglDrm& ctx) {
  drain_drm_events(ctx);

  if (ctx.pageflip_enabled && ctx.pageflip_pending) {
    wait_pageflip(ctx, 16);
    if (ctx.pageflip_pending) {
      // If the event doesn't arrive, skipping causes a static frame. Switch to modeset fallback
      // immediately to keep live output.
      if (ctx.debug) {
        std::fprintf(stderr, "[drm_gbm_egl] pageflip pending too long, switching to drmModeSetCrtc fallback\n");
      }
      ctx.pageflip_enabled = false;
      ctx.pageflip_pending = false;
      ctx.pageflip_timeouts = 0;
      ctx.pageflip_dropped = 0;
      if (ctx.gbm_surf && ctx.prev_bo) {
        gbm_surface_release_buffer(ctx.gbm_surf, ctx.prev_bo);
        ctx.prev_bo = nullptr;
      }
      if (ctx.gbm_surf && ctx.prev_bo2) {
        gbm_surface_release_buffer(ctx.gbm_surf, ctx.prev_bo2);
        ctx.prev_bo2 = nullptr;
      }
      if (ctx.gbm_surf && ctx.cur_bo) {
        gbm_surface_release_buffer(ctx.gbm_surf, ctx.cur_bo);
        ctx.cur_bo = nullptr;
      }
    }
  }

  eglSwapBuffers(ctx.egl_display, ctx.egl_surface);

  gbm_bo* bo = gbm_surface_lock_front_buffer(ctx.gbm_surf);
  if (!bo) return false;

  bool was_modeset = ctx.modeset_done;

  uint32_t fb_id = 0;
  if (!drm_set_crtc_for_bo(ctx, bo, fb_id)) {
    gbm_surface_release_buffer(ctx.gbm_surf, bo);
    return false;
  }

  if (!was_modeset && ctx.modeset_done) {
    if (ctx.prev_bo) gbm_surface_release_buffer(ctx.gbm_surf, ctx.prev_bo);
    ctx.prev_bo = bo;
    return true;
  }

  if (!ctx.pageflip_use_event) {
    int ret_noev = drmModePageFlip(ctx.drm_fd, ctx.crtc_id, fb_id, 0, nullptr);
    if (ret_noev) {
      if (errno == EBUSY) {
        if (ctx.debug) {
          std::fprintf(stderr, "[drm_gbm_egl] drmModePageFlip (no-event fallback) EBUSY, dropping frame\n");
        }
        gbm_surface_release_buffer(ctx.gbm_surf, bo);
        ctx.pageflip_dropped++;
        return true;
      }
      std::fprintf(stderr, "[drm_gbm_egl] drmModePageFlip (no-event fallback) failed: %s\n", std::strerror(errno));
      gbm_surface_release_buffer(ctx.gbm_surf, bo);
      return false;
    }
    ctx.pageflip_submitted++;
    // With no completion events, keep 2 scanout buffers and only release the oldest.
    if (ctx.gbm_surf && ctx.prev_bo2) {
      gbm_surface_release_buffer(ctx.gbm_surf, ctx.prev_bo2);
      ctx.prev_bo2 = nullptr;
    }
    ctx.prev_bo2 = ctx.prev_bo;
    ctx.prev_bo = bo;
    ctx.cur_bo = nullptr;
    ctx.pageflip_pending = false;
    return true;
  }

  if (!ctx.pageflip_enabled) {
    static uint64_t fallback_frames = 0;
    fallback_frames++;
    int set_ret = drmModeSetCrtc(ctx.drm_fd, ctx.crtc_id, fb_id, 0, 0, &ctx.connector_id, 1, &ctx.mode);
    if (set_ret) {
      std::fprintf(stderr, "[drm_gbm_egl] drmModeSetCrtc (fallback) failed: %s\n", std::strerror(errno));
      gbm_surface_release_buffer(ctx.gbm_surf, bo);
      return false;
    }
    if (ctx.debug && (fallback_frames % 120) == 0) {
      std::fprintf(stderr, "[drm_gbm_egl] fallback frames=%llu fb_id=%u bo=%p\n",
                   (unsigned long long)fallback_frames, fb_id, (void*)bo);
    }
    if (ctx.debug && fallback_frames <= 30) {
      std::fprintf(stderr, "[drm_gbm_egl] fallback early: frame=%llu fb_id=%u bo=%p\n",
                   (unsigned long long)fallback_frames, fb_id, (void*)bo);
    }
    // In SetCrtc fallback, free the previous BO immediately to avoid starving the GBM surface.
    if (ctx.gbm_surf && ctx.prev_bo) {
      gbm_surface_release_buffer(ctx.gbm_surf, ctx.prev_bo);
      ctx.prev_bo = nullptr;
    }
    if (ctx.gbm_surf && ctx.prev_bo2) {
      gbm_surface_release_buffer(ctx.gbm_surf, ctx.prev_bo2);
      ctx.prev_bo2 = nullptr;
    }
    ctx.prev_bo = bo;
    return true;
  }

  if (ctx.pageflip_pending) {
    wait_pageflip(ctx, 16);
    if (!ctx.pageflip_pending) {
      ctx.cur_bo = bo;
      ctx.pageflip_pending = true;
      int ret2 = drmModePageFlip(ctx.drm_fd, ctx.crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, &ctx);
      if (ret2) {
        ctx.pageflip_pending = false;
        std::fprintf(stderr, "[drm_gbm_egl] drmModePageFlip failed: %s\n", std::strerror(errno));
        gbm_surface_release_buffer(ctx.gbm_surf, bo);
        return false;
      }
      ctx.pageflip_submitted++;
      return true;
    }
    ctx.pageflip_dropped++;
    gbm_surface_release_buffer(ctx.gbm_surf, bo);
    return true;
  }

  ctx.cur_bo = bo;
  ctx.pageflip_pending = true;
  int ret = drmModePageFlip(ctx.drm_fd, ctx.crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, &ctx);
  if (ret) {
    ctx.pageflip_pending = false;
    std::fprintf(stderr, "[drm_gbm_egl] drmModePageFlip failed: %s\n", std::strerror(errno));
    gbm_surface_release_buffer(ctx.gbm_surf, bo);
    return false;
  }

  ctx.pageflip_submitted++;

  return true;
}

void destroy_drm_gbm_egl(GbmEglDrm& ctx) {
  if (ctx.egl_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(ctx.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx.egl_surface != EGL_NO_SURFACE) eglDestroySurface(ctx.egl_display, ctx.egl_surface);
    if (ctx.egl_context != EGL_NO_CONTEXT) eglDestroyContext(ctx.egl_display, ctx.egl_context);
    eglTerminate(ctx.egl_display);
  }
  ctx.egl_surface = EGL_NO_SURFACE;
  ctx.egl_context = EGL_NO_CONTEXT;
  ctx.egl_display = EGL_NO_DISPLAY;

  if (ctx.gbm_surf && ctx.prev_bo2) {
    gbm_surface_release_buffer(ctx.gbm_surf, ctx.prev_bo2);
    ctx.prev_bo2 = nullptr;
  }
  if (ctx.gbm_surf && ctx.prev_bo) {
    gbm_surface_release_buffer(ctx.gbm_surf, ctx.prev_bo);
    ctx.prev_bo = nullptr;
  }

  if (ctx.gbm_surf) gbm_surface_destroy(ctx.gbm_surf);
  if (ctx.gbm_dev) gbm_device_destroy(ctx.gbm_dev);
  ctx.gbm_surf = nullptr;
  ctx.gbm_dev = nullptr;

  if (ctx.drm_fd >= 0) close(ctx.drm_fd);
  ctx.drm_fd = -1;
}
