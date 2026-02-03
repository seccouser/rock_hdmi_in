#include "drm_gbm_egl.h"
#include "v4l2_capture.h"
#include "shader_utils.h"

#include <GLES2/gl2.h>
 #include <GLES2/gl2ext.h>

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <cctype>
#include <unordered_map>
#include <unistd.h>
#include <csignal>
#include <time.h>
 #include <EGL/eglext.h>
 #include <drm_fourcc.h>

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_sigint_count = 0;

static void on_sigalrm(int) {
  std::_Exit(130);
}

static void on_sigint(int) {
  g_sigint_count++;
  g_running = 0;
  if (g_sigint_count >= 2) {
    std::_Exit(130);
  }

  alarm(1);
}

static std::string read_text_file(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return std::string();
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string dirname_of(const std::string& p) {
  size_t pos = p.find_last_of('/');
  if (pos == std::string::npos) return std::string(".");
  if (pos == 0) return std::string("/");
  return p.substr(0, pos);
}

static std::string get_exe_dir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return std::string(".");
  buf[n] = '\0';
  return dirname_of(std::string(buf));
}

static std::string load_shader_from_dir(const std::string& shader_dir, const char* name) {
  std::string p = shader_dir;
  if (!p.empty() && p.back() != '/') p.push_back('/');
  p += name;
  return read_text_file(p);
}

static std::string load_shader(const std::string& shader_dir, const std::string& name_or_path) {
  if (!name_or_path.empty() && name_or_path[0] == '/') {
    return read_text_file(name_or_path);
  }
  return load_shader_from_dir(shader_dir, name_or_path.c_str());
}

int main(int argc, char** argv) {
  {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
  }

  {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigalrm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, nullptr);
  }

  std::string video_dev = "/dev/video0";
  std::string drm_dev = "/dev/dri/card0";
  uint32_t cap_w = 0;
  uint32_t cap_h = 0;

  std::string shader_dir;
  std::string vs_file;
  std::string fs_file;
  std::string post_vs_file;
  std::string post_fs_file;

  bool use_config = true;
  std::string config_file;

  std::string profile_name;
  std::string profile_file;

  bool nv21 = false;
  bool debug = false;
  bool disable_zero_copy = false;
  bool test_clear = false;
  bool flip_y = false;
  bool dmabuf_uv_ra = false;
  bool enable_subpixel = false;
  uint32_t buffers = 4;

  int sub_mx = 4;
  int sub_my = 4;
  int sub_views = 7;
  int sub_wz = 4;
  int sub_wn = 5;
  int sub_test = 0;
  int sub_left = 1;
  int sub_mstart = 0;
  int sub_hq = 0;
  bool sub_left_overridden = false;

  auto trim_in_place = [](std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b])) b++;
    size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    s = s.substr(b, e - b);
  };

  auto default_config_path = [&]() -> std::string {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return std::string();
    return std::string(home) + "/.config/3dplayer.conf";
  };

  auto load_config_file = [&](const std::string& path) -> bool {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::unordered_map<std::string, int*> mi{
        {"mx", &sub_mx},
        {"my", &sub_my},
        {"views", &sub_views},
        {"wz", &sub_wz},
        {"wn", &sub_wn},
        {"test", &sub_test},
        {"left", &sub_left},
        {"mstart", &sub_mstart},
        {"hq", &sub_hq},
    };
    std::unordered_map<std::string, bool*> mb{
        {"flip_y", &flip_y},
        {"nv21", &nv21},
        {"dmabuf_uv_ra", &dmabuf_uv_ra},
        {"subpixel", &enable_subpixel},
    };

    std::string line;
    while (std::getline(f, line)) {
      trim_in_place(line);
      if (line.empty()) continue;
      if (line[0] == '#') continue;
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string key = line.substr(0, eq);
      std::string val = line.substr(eq + 1);
      trim_in_place(key);
      trim_in_place(val);

      if (key == "video_dev") {
        video_dev = val;
        continue;
      }
      if (key == "drm_dev") {
        drm_dev = val;
        continue;
      }
      if (key == "shader_dir") {
        shader_dir = val;
        continue;
      }
      if (key == "buffers") {
        buffers = (uint32_t)std::strtoul(val.c_str(), nullptr, 10);
        continue;
      }

      auto itb = mb.find(key);
      if (itb != mb.end()) {
        const int v = std::atoi(val.c_str());
        *(itb->second) = (v != 0);
        continue;
      }
      auto iti = mi.find(key);
      if (iti != mi.end()) {
        *(iti->second) = std::atoi(val.c_str());
        continue;
      }
    }
    return true;
  };

  auto load_profile_into_subparams = [&](const std::string& path) -> bool {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::unordered_map<std::string, int*> m{
        {"mx", &sub_mx},
        {"my", &sub_my},
        {"views", &sub_views},
        {"wz", &sub_wz},
        {"wn", &sub_wn},
        {"test", &sub_test},
        {"left", &sub_left},
        {"mstart", &sub_mstart},
        {"hq", &sub_hq},
    };
    std::unordered_map<std::string, bool*> mb{
        {"flip_y", &flip_y},
        {"nv21", &nv21},
        {"dmabuf_uv_ra", &dmabuf_uv_ra},
        {"subpixel", &enable_subpixel},
    };
    std::string line;
    while (std::getline(f, line)) {
      trim_in_place(line);
      if (line.empty()) continue;
      if (line[0] == '#') continue;
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string key = line.substr(0, eq);
      std::string val = line.substr(eq + 1);
      trim_in_place(key);
      trim_in_place(val);
      auto itb = mb.find(key);
      if (itb != mb.end()) {
        const int v = std::atoi(val.c_str());
        *(itb->second) = (v != 0);
        continue;
      }
      auto it = m.find(key);
      if (it == m.end()) continue;
      *(it->second) = std::atoi(val.c_str());
    }
    return true;
  };

  // Optional positional video device for backwards compatibility:
  //   rock5b_hdmiin_gl /dev/video0 [options]
  // Do NOT scan all args (otherwise option values like profile names could be mistaken).
  int arg_start = 1;
  if (argc >= 2 && argv[1] && argv[1][0] != '-') {
    video_dev = argv[1];
    arg_start = 2;
  }

  if (shader_dir.empty()) {
    const std::string exe_dir = get_exe_dir();
    if (!exe_dir.empty()) {
      shader_dir = exe_dir + "/../shaders";
    } else {
      shader_dir = "./shaders";
    }
  }

  for (int i = arg_start; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--config" && (i + 1) < argc) {
      config_file = argv[++i];
    } else if (a == "--no-config") {
      use_config = false;
    }
  }

  if (use_config) {
    if (config_file.empty()) config_file = default_config_path();
    if (!config_file.empty()) {
      (void)load_config_file(config_file);
    }
  }

  for (int i = arg_start; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--shader-dir" && (i + 1) < argc) {
      shader_dir = argv[++i];
    } else if (a == "--profile" && (i + 1) < argc) {
      profile_name = argv[++i];
    } else if (a == "--profile-file" && (i + 1) < argc) {
      profile_file = argv[++i];
    }
  }

  if (!profile_file.empty()) {
    if (!load_profile_into_subparams(profile_file)) {
      std::fprintf(stderr, "[rock5b_hdmiin_gl] failed to load profile file: %s\n", profile_file.c_str());
      return 2;
    }
  } else if (!profile_name.empty()) {
    const std::string path = shader_dir + "/profiles/" + profile_name + ".profile";
    if (!load_profile_into_subparams(path)) {
      std::fprintf(stderr, "[rock5b_hdmiin_gl] failed to load profile: %s (%s)\n", profile_name.c_str(), path.c_str());
      return 2;
    }
  }

  for (int i = arg_start; i < argc; i++) {
    if (std::string(argv[i]) == "--nv21") {
      nv21 = true;
    } else if (std::string(argv[i]) == "--debug") {
      debug = true;
    } else if (std::string(argv[i]) == "--video" && (i + 1) < argc) {
      video_dev = argv[++i];
    } else if (std::string(argv[i]) == "--drm" && (i + 1) < argc) {
      drm_dev = argv[++i];
    } else if (std::string(argv[i]) == "--shader-dir" && (i + 1) < argc) {
      shader_dir = argv[++i];
    } else if (std::string(argv[i]) == "--profile" && (i + 1) < argc) {
      i++;
    } else if (std::string(argv[i]) == "--profile-file" && (i + 1) < argc) {
      i++;
    } else if (std::string(argv[i]) == "--fs" && (i + 1) < argc) {
      fs_file = argv[++i];
    } else if (std::string(argv[i]) == "--vs" && (i + 1) < argc) {
      vs_file = argv[++i];
    } else if (std::string(argv[i]) == "--post-vs" && (i + 1) < argc) {
      post_vs_file = argv[++i];
    } else if (std::string(argv[i]) == "--post-fs" && (i + 1) < argc) {
      post_fs_file = argv[++i];
    } else if (std::string(argv[i]) == "--no-zero-copy") {
      disable_zero_copy = true;
    } else if (std::string(argv[i]) == "--test-clear") {
      test_clear = true;
    } else if (std::string(argv[i]) == "--flip-y") {
      flip_y = true;
    } else if (std::string(argv[i]) == "--dmabuf-uv-ra") {
      dmabuf_uv_ra = true;
    } else if (std::string(argv[i]) == "--subpixel") {
      enable_subpixel = true;
    } else if (std::string(argv[i]) == "--mx" && (i + 1) < argc) {
      sub_mx = std::atoi(argv[++i]);
    } else if (std::string(argv[i]) == "--my" && (i + 1) < argc) {
      sub_my = std::atoi(argv[++i]);
    } else if (std::string(argv[i]) == "--views" && (i + 1) < argc) {
      sub_views = std::atoi(argv[++i]);
    } else if (std::string(argv[i]) == "--wz" && (i + 1) < argc) {
      sub_wz = std::atoi(argv[++i]);
    } else if (std::string(argv[i]) == "--wn" && (i + 1) < argc) {
      sub_wn = std::atoi(argv[++i]);
    } else if (std::string(argv[i]) == "--test" && (i + 1) < argc) {
      sub_test = std::atoi(argv[++i]);
    } else if (std::string(argv[i]) == "--left" && (i + 1) < argc) {
      sub_left = std::atoi(argv[++i]);
      sub_left_overridden = true;
    } else if (std::string(argv[i]) == "--mstart" && (i + 1) < argc) {
      sub_mstart = std::atoi(argv[++i]);
    } else if (std::string(argv[i]) == "--hq" && (i + 1) < argc) {
      sub_hq = std::atoi(argv[++i]);
    } else if (std::string(argv[i]) == "--buffers" && (i + 1) < argc) {
      buffers = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
    } else if (std::string(argv[i]) == "--w" && (i + 1) < argc) {
      cap_w = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
    } else if (std::string(argv[i]) == "--h" && (i + 1) < argc) {
      cap_h = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
    }
  }

  GbmEglDrm gfx{};
  gfx.debug = debug;
  std::fprintf(stderr, "[rock5b_hdmiin_gl] init DRM/GBM/EGL on %s\n", drm_dev.c_str());
  if (!init_drm_gbm_egl(gfx, drm_dev.c_str())) {
    std::fprintf(stderr, "[rock5b_hdmiin_gl] init_drm_gbm_egl failed\n");
    return 1;
  }
  if (!drm_gbm_egl_make_current(gfx)) {
    std::fprintf(stderr, "[rock5b_hdmiin_gl] eglMakeCurrent failed\n");
    return 1;
  }

  const char* egl_ext = eglQueryString(gfx.egl_display, EGL_EXTENSIONS);
  bool egl_has_dmabuf_import = false;
  if (egl_ext) {
    if (debug) std::fprintf(stderr, "[rock5b_hdmiin_gl] EGL_EXTENSIONS=%s\n", egl_ext);
    egl_has_dmabuf_import = (std::strstr(egl_ext, "EGL_EXT_image_dma_buf_import") != nullptr);
  }

  V4L2Capture cap;
  cap.set_debug(debug);
  cap.set_nv12_uv_swap(nv21);
  cap.set_request_buffer_count(buffers);
  std::fprintf(stderr, "[rock5b_hdmiin_gl] open V4L2 device %s\n", video_dev.c_str());
  if (!cap.open_device(video_dev)) {
    std::fprintf(stderr, "[rock5b_hdmiin_gl] open_device failed: %s\n", std::strerror(errno));
    return 2;
  }
  if (!cap.configure(cap_w, cap_h)) {
    std::fprintf(stderr, "[rock5b_hdmiin_gl] configure failed (requested %ux%u)\n", cap_w, cap_h);
    return 3;
  }
  if (debug) std::fprintf(stderr, "[rock5b_hdmiin_gl] V4L2 dmabuf_export_supported=%d\n", cap.dmabuf_export_supported() ? 1 : 0);
  std::fprintf(stderr, "[rock5b_hdmiin_gl] V4L2 configured: %ux%u fourcc=0x%08x\n", cap.width(), cap.height(), cap.fourcc());
  if (!cap.start()) {
    std::fprintf(stderr, "[rock5b_hdmiin_gl] start capture failed\n");
    return 4;
  }

  bool use_nv12 = (cap.fourcc() == 0x3231564e);
  bool use_zero_copy = false;
  if (use_nv12 && egl_has_dmabuf_import && cap.dmabuf_export_supported()) {
    use_zero_copy = true;
    if (debug) std::fprintf(stderr, "[rock5b_hdmiin_gl] zero-copy path enabled (DMABUF + EGLImage)\n");
  }
  if (disable_zero_copy) use_zero_copy = false;

  if (shader_dir.empty()) {
    const std::string exe_dir = get_exe_dir();
    shader_dir = exe_dir + "/../shaders";
  }

  if (vs_file.empty()) vs_file = "fullscreen.vs.glsl";
  if (fs_file.empty()) {
    if (use_nv12) {
      fs_file = use_zero_copy ? "nv12_dmabuf.fs.glsl" : "nv12.fs.glsl";
    } else {
      fs_file = "blit.fs.glsl";
    }
  }
  if (post_vs_file.empty()) post_vs_file = "fullscreen.vs.glsl";

  if (post_fs_file.empty() && enable_subpixel) {
    post_fs_file = "mosaic_subpixel.fs.glsl";
  }

  // Debug: show the pre-pass/FBO output directly without mosaic logic.
  if (enable_subpixel && sub_test == 2) {
    post_fs_file = "blit.fs.glsl";
  }

  // If the post-pass uses vertically flipped v_uv, raster indexing must be flipped too.
  // For two-pass/subpixel, the "upright" output uses vertically flipped UVs to compensate
  // for FBO texture orientation.
  if (enable_subpixel && !flip_y && !sub_left_overridden) {
    // mosaic_subpixel uses both gl_FragCoord (screen-space) and v_uv (texture-space).
    // When flipping v_uv in the post-pass, we must flip the raster indexing as well,
    // otherwise channel selection will be misaligned and colors will be wrong.
    sub_left = 0;
  }

  const bool two_pass = !post_fs_file.empty();

  auto load_and_build_program = [&](const std::string& vs_name, const std::string& fs_name) -> GLuint {
    std::string vs_src = load_shader(shader_dir, vs_name);
    std::string fs_src = load_shader(shader_dir, fs_name);
    if (vs_src.empty() || fs_src.empty()) {
      std::fprintf(stderr, "[rock5b_hdmiin_gl] failed to load shaders from %s\n", shader_dir.c_str());
      if (debug) {
        std::fprintf(stderr, "[rock5b_hdmiin_gl] requested vs=%s fs=%s\n", vs_name.c_str(), fs_name.c_str());
      }
      return 0;
    }
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src.c_str());
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src.c_str());
    if (!vs || !fs) return 0;
    return link_program(vs, fs);
  };

  GLuint prog_pre = 0;
  GLuint prog_post = 0;

  if (!two_pass) {
    prog_pre = load_and_build_program(vs_file, fs_file);
    if (!prog_pre) {
      std::fprintf(stderr, "[rock5b_hdmiin_gl] program link failed\n");
      return 6;
    }
  } else {
    prog_pre = load_and_build_program("fullscreen.vs.glsl", use_nv12 ? (use_zero_copy ? "nv12_dmabuf.fs.glsl" : "nv12.fs.glsl") : "blit.fs.glsl");
    prog_post = load_and_build_program(post_vs_file, post_fs_file);
    if (!prog_pre || !prog_post) {
      std::fprintf(stderr, "[rock5b_hdmiin_gl] program link failed\n");
      return 6;
    }
  }

  GLint post_loc_mx = -1;
  GLint post_loc_my = -1;
  GLint post_loc_views = -1;
  GLint post_loc_wz = -1;
  GLint post_loc_wn = -1;
  GLint post_loc_test = -1;
  GLint post_loc_left = -1;
  GLint post_loc_mstart = -1;
  GLint post_loc_hq = -1;
  GLint post_loc_res = -1;
  if (two_pass) {
    post_loc_mx = glGetUniformLocation(prog_post, "mx");
    post_loc_my = glGetUniformLocation(prog_post, "my");
    post_loc_views = glGetUniformLocation(prog_post, "views");
    post_loc_wz = glGetUniformLocation(prog_post, "wz");
    post_loc_wn = glGetUniformLocation(prog_post, "wn");
    post_loc_test = glGetUniformLocation(prog_post, "test");
    post_loc_left = glGetUniformLocation(prog_post, "left");
    post_loc_mstart = glGetUniformLocation(prog_post, "mstart");
    post_loc_hq = glGetUniformLocation(prog_post, "hq");
    post_loc_res = glGetUniformLocation(prog_post, "u_resolution");

    glUseProgram(prog_post);
    if (post_loc_mx >= 0) glUniform1i(post_loc_mx, sub_mx);
    if (post_loc_my >= 0) glUniform1i(post_loc_my, sub_my);
    if (post_loc_views >= 0) glUniform1i(post_loc_views, sub_views);
    if (post_loc_wz >= 0) glUniform1i(post_loc_wz, sub_wz);
    if (post_loc_wn >= 0) glUniform1i(post_loc_wn, sub_wn);
    if (post_loc_test >= 0) glUniform1i(post_loc_test, sub_test);
    if (post_loc_left >= 0) glUniform1i(post_loc_left, sub_left);
    if (post_loc_mstart >= 0) glUniform1i(post_loc_mstart, sub_mstart);
    if (post_loc_hq >= 0) glUniform1i(post_loc_hq, sub_hq);
    if (post_loc_res >= 0) glUniform2i(post_loc_res, (int)gfx.mode_hdisplay, (int)gfx.mode_vdisplay);
  }

  GLint a_pos_pre = glGetAttribLocation(prog_pre, "a_pos");
  GLint a_uv_pre = glGetAttribLocation(prog_pre, "a_uv");
  GLint u_tex_pre = use_nv12 ? -1 : glGetUniformLocation(prog_pre, "u_tex");
  GLint u_tex_y_pre = use_nv12 ? glGetUniformLocation(prog_pre, "u_tex_y") : -1;
  GLint u_tex_uv_pre = use_nv12 ? glGetUniformLocation(prog_pre, "u_tex_uv") : -1;
  GLint u_uvSwap_pre = use_nv12 ? glGetUniformLocation(prog_pre, "u_uvSwap") : -1;
  GLint u_uvRA_pre = use_nv12 ? glGetUniformLocation(prog_pre, "u_uvRA") : -1;

  GLint a_pos_post = -1;
  GLint a_uv_post = -1;
  GLint u_tex_post = -1;
  if (two_pass) {
    a_pos_post = glGetAttribLocation(prog_post, "a_pos");
    a_uv_post = glGetAttribLocation(prog_post, "a_uv");
    u_tex_post = glGetUniformLocation(prog_post, "u_tex");
  }

  if (debug) {
    std::fprintf(stderr, "[rock5b_hdmiin_gl] pipeline: nv12=%d zero_copy=%d two_pass=%d\n", use_nv12 ? 1 : 0, use_zero_copy ? 1 : 0, two_pass ? 1 : 0);
    std::fprintf(stderr, "[rock5b_hdmiin_gl] pre a_pos=%d a_uv=%d u_tex=%d u_tex_y=%d u_tex_uv=%d u_uvSwap=%d\n",
                 (int)a_pos_pre, (int)a_uv_pre, (int)u_tex_pre, (int)u_tex_y_pre, (int)u_tex_uv_pre, (int)u_uvSwap_pre);
    std::fprintf(stderr, "[rock5b_hdmiin_gl] pre u_uvRA=%d (dmabuf_uv_ra=%d)\n", (int)u_uvRA_pre, dmabuf_uv_ra ? 1 : 0);
    if (two_pass) {
      std::fprintf(stderr, "[rock5b_hdmiin_gl] post a_pos=%d a_uv=%d u_tex=%d\n", (int)a_pos_post, (int)a_uv_post, (int)u_tex_post);
    }
  }

  GLuint tex = 0;
  GLuint tex_y = 0;
  GLuint tex_uv = 0;

  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ptr = nullptr;
  PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ptr = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ptr = nullptr;

  std::vector<EGLImageKHR> y_images;
  std::vector<EGLImageKHR> uv_images;
  std::vector<GLuint> y_texs;
  std::vector<GLuint> uv_texs;

  if (!use_nv12) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    if (!use_zero_copy) {
      glGenTextures(1, &tex_y);
      glBindTexture(GL_TEXTURE_2D, tex_y);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

      glGenTextures(1, &tex_uv);
      glBindTexture(GL_TEXTURE_2D, tex_uv);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
      glEGLImageTargetTexture2DOES_ptr = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
      eglCreateImageKHR_ptr = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
      eglDestroyImageKHR_ptr = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
      if (!glEGLImageTargetTexture2DOES_ptr || !eglCreateImageKHR_ptr || !eglDestroyImageKHR_ptr) {
        std::fprintf(stderr, "[rock5b_hdmiin_gl] zero-copy requested but EGL/GL entrypoints missing\n");
        use_zero_copy = false;
      }
    }
  }

  bool tex_alloc = false;
  uint32_t tex_w = 0;
  uint32_t tex_h = 0;

  GLuint fbo = 0;
  GLuint fbo_tex = 0;
  bool fbo_alloc = false;
  uint32_t fbo_w = 0;
  uint32_t fbo_h = 0;

  GLuint cur_rgb_tex = 0;
  GLuint cur_y_tex = 0;
  GLuint cur_uv_tex = 0;

  glViewport(0, 0, (GLsizei)gfx.mode_hdisplay, (GLsizei)gfx.mode_vdisplay);

  const GLfloat verts[] = {
      -1.0f, -1.0f,
       1.0f, -1.0f,
      -1.0f,  1.0f,
       1.0f,  1.0f,
  };

  const GLfloat uvs_default[] = {
      0.0f, 1.0f,
      1.0f, 1.0f,
      0.0f, 0.0f,
      1.0f, 0.0f,
  };
  const GLfloat uvs_flipy[] = {
      0.0f, 0.0f,
      1.0f, 0.0f,
      0.0f, 1.0f,
      1.0f, 1.0f,
  };
  // One-pass: uvs_default is upright.
  // Two-pass: sampling the FBO texture needs a vertical flip for upright output.
  const GLfloat* uvs_upright = two_pass ? uvs_flipy : uvs_default;
  const GLfloat* uvs_flipped = two_pass ? uvs_default : uvs_flipy;

  // In one-pass mode, the pre shader outputs directly to the screen, so mapping must be
  // applied here. In two-pass mode, mapping is applied only in the post pass.
  const GLfloat* uvs_pre = two_pass ? uvs_default : (flip_y ? uvs_flipped : uvs_upright);
  const GLfloat* uvs_post = flip_y ? uvs_flipped : uvs_upright;

  if (debug) {
    std::fprintf(stderr, "[rock5b_hdmiin_gl] flip_y=%d one_pass_uv=%s post_uv=%s\n",
                 flip_y ? 1 : 0,
                 (!two_pass ? (flip_y ? "flipped" : "upright") : "n/a"),
                 (flip_y ? "flipped" : "upright"));
  }

  std::fprintf(stderr, "[rock5b_hdmiin_gl] entering render loop\n");
  uint64_t frame_counter = 0;
  uint64_t last_frame_counter = 0;
  uint64_t last_flip_submitted = gfx.pageflip_submitted;
  uint64_t last_flip_completed = gfx.pageflip_completed;
  uint64_t last_flip_dropped = gfx.pageflip_dropped;
  uint64_t last_seen_flip_completed = gfx.pageflip_completed;
  int displayed_v4l2_index = -1;
  int pending_v4l2_index = -1;
  bool first_frame_gl_checked = false;
  uint64_t no_frame_ticks = 0;
  uint32_t last_dbg_frame_index = 0;
  int64_t last_dbg_frame_ts_us = 0;
  timespec last_stat{};
  clock_gettime(CLOCK_MONOTONIC, &last_stat);
  uint32_t early_dbg_frames = 0;
  while (g_running) {
    if (test_clear) {
      frame_counter++;
      const float t = (float)(frame_counter % 120) / 120.0f;
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glViewport(0, 0, (GLsizei)gfx.mode_hdisplay, (GLsizei)gfx.mode_vdisplay);
      glClearColor(t, 0.2f, 1.0f - t, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      if (!drm_gbm_egl_swap_buffers(gfx)) {
        std::fprintf(stderr, "[rock5b_hdmiin_gl] swap_buffers failed\n");
        break;
      }
      glFlush();
      usleep(16000);
      continue;
    }

    if (use_zero_copy) {
      while (gfx.pageflip_completed > last_seen_flip_completed) {
        last_seen_flip_completed++;
        if (displayed_v4l2_index >= 0) {
          V4L2Frame rel;
          rel.needs_release = true;
          rel.index = (uint32_t)displayed_v4l2_index;
          if (!cap.release_frame(rel)) {
            std::fprintf(stderr, "[rock5b_hdmiin_gl] release_frame failed\n");
            break;
          }
        }
        displayed_v4l2_index = pending_v4l2_index;
        pending_v4l2_index = -1;
      }
    }

    V4L2Frame frame;
    if (!cap.acquire_frame(frame)) {
      std::fprintf(stderr, "[rock5b_hdmiin_gl] cap.acquire_frame failed\n");
      break;
    }

    const bool dbg_early = debug && (early_dbg_frames < 60);
    if (dbg_early) {
      std::fprintf(stderr, "[rock5b_hdmiin_gl] dbg stage=acquired needs_release=%d idx=%u\n", frame.needs_release ? 1 : 0, frame.index);
      std::fflush(stderr);
    }

    if (debug && early_dbg_frames < 60) {
      early_dbg_frames++;
      int64_t cur_ts_us = (int64_t)frame.ts_sec * 1000000LL + (int64_t)frame.ts_usec;
      uint32_t y_fp = 0;
      uint32_t uv_fp = 0;
      if (frame.plane0 && frame.y_stride && frame.height) {
        const uint8_t* p = frame.plane0;
        const uint32_t stride = frame.y_stride;
        const uint32_t h = frame.height;
        const uint32_t w = frame.width;
        const uint32_t off0 = 0;
        const uint32_t off1 = (h / 2) * stride + (w / 2);
        const uint32_t off2 = (h - 1) * stride;
        const uint32_t off3 = (h - 1) * stride + (w - 1);
        y_fp = (uint32_t)p[off0] | ((uint32_t)p[off1] << 8) | ((uint32_t)p[off2] << 16) | ((uint32_t)p[off3] << 24);
      }
      if (frame.plane1 && frame.uv_stride && frame.height) {
        const uint8_t* p = frame.plane1;
        const uint32_t stride = frame.uv_stride;
        const uint32_t h = frame.height / 2;
        const uint32_t w = frame.width;
        const uint32_t off0 = 0;
        const uint32_t off1 = (h / 2) * stride + (w / 2);
        const uint32_t off2 = (h - 1) * stride;
        const uint32_t off3 = (h - 1) * stride + (w - 2);
        uv_fp = (uint32_t)p[off0] | ((uint32_t)p[off1] << 8) | ((uint32_t)p[off2] << 16) | ((uint32_t)p[off3] << 24);
      }
      std::fprintf(stderr, "[rock5b_hdmiin_gl] cap early: needs_release=%d idx=%u ts_us=%lld yfp=0x%08x uvfp=0x%08x\n",
                   frame.needs_release ? 1 : 0,
                   (unsigned)frame.index,
                   (long long)cur_ts_us,
                   (unsigned)y_fp,
                   (unsigned)uv_fp);
    }

    if (debug) {
      if (!frame.needs_release) {
        no_frame_ticks++;
        if ((no_frame_ticks % 60) == 0) {
          std::fprintf(stderr, "[rock5b_hdmiin_gl] waiting for frames...\n");
        }
      } else {
        no_frame_ticks = 0;
      }
    }

    if (debug) {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      double dt = (double)(now.tv_sec - last_stat.tv_sec) + (double)(now.tv_nsec - last_stat.tv_nsec) / 1e9;
      if (dt >= 1.0) {
        uint64_t df = frame_counter - last_frame_counter;
        uint64_t dsub = gfx.pageflip_submitted - last_flip_submitted;
        uint64_t dcom = gfx.pageflip_completed - last_flip_completed;
        uint64_t ddrop = gfx.pageflip_dropped - last_flip_dropped;
        int64_t cur_ts_us = (int64_t)frame.ts_sec * 1000000LL + (int64_t)frame.ts_usec;
        int64_t dts_us = (last_dbg_frame_ts_us == 0) ? 0 : (cur_ts_us - last_dbg_frame_ts_us);
        std::fprintf(stderr, "[rock5b_hdmiin_gl] fps=%.1f flips(sub=%llu com=%llu drop=%llu)\n",
                     (double)df / dt,
                     (unsigned long long)dsub,
                     (unsigned long long)dcom,
                     (unsigned long long)ddrop);
        std::fprintf(stderr, "[rock5b_hdmiin_gl] cap dbg: needs_release=%d idx=%u ts_us=%lld dts_us=%lld\n",
                     frame.needs_release ? 1 : 0,
                     (unsigned)frame.index,
                     (long long)cur_ts_us,
                     (long long)dts_us);
        last_stat = now;
        last_frame_counter = frame_counter;
        last_flip_submitted = gfx.pageflip_submitted;
        last_flip_completed = gfx.pageflip_completed;
        last_flip_dropped = gfx.pageflip_dropped;
        last_dbg_frame_index = frame.index;
        last_dbg_frame_ts_us = cur_ts_us;
      }
    }

    if (frame.needs_release) {
      frame_counter++;
      if (debug && (frame_counter % 60) == 0 && frame.ts_sec != 0) {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t now_us = (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_nsec / 1000LL;
        int64_t cap_us = frame.ts_sec * 1000000LL + frame.ts_usec;
        std::fprintf(stderr, "[rock5b_hdmiin_gl] capture_age_ms=%.1f\n", (double)(now_us - cap_us) / 1000.0);
      }
    }

    if (!frame.needs_release && frame.data.empty() && !use_nv12) {
      if (!drm_gbm_egl_swap_buffers(gfx)) {
        std::fprintf(stderr, "[rock5b_hdmiin_gl] swap_buffers failed\n");
        break;
      }
      continue;
    }

    if (use_nv12) {
      if (!frame.needs_release) {
        if (!drm_gbm_egl_swap_buffers(gfx)) {
          std::fprintf(stderr, "[rock5b_hdmiin_gl] swap_buffers failed\n");
          break;
        }
        continue;
      }

      if (!use_zero_copy) {
        cur_y_tex = tex_y;
        cur_uv_tex = tex_uv;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_y);
        if (!tex_alloc || tex_w != frame.width || tex_h != frame.height) {
          glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (GLsizei)frame.width, (GLsizei)frame.height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)frame.width, (GLsizei)frame.height, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.plane0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_uv);
        if (!tex_alloc || tex_w != frame.width || tex_h != frame.height) {
          glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, (GLsizei)(frame.width / 2), (GLsizei)(frame.height / 2), 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, nullptr);
          tex_alloc = true;
          tex_w = frame.width;
          tex_h = frame.height;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)(frame.width / 2), (GLsizei)(frame.height / 2), GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, frame.plane1);
      } else {
        if (y_images.empty()) {
          const size_t nbuf = (size_t)cap.buffer_count();
          y_images.assign(nbuf, EGL_NO_IMAGE_KHR);
          uv_images.assign(nbuf, EGL_NO_IMAGE_KHR);
          y_texs.assign(nbuf, 0);
          uv_texs.assign(nbuf, 0);

          for (size_t i = 0; i < nbuf; i++) {
            int fd = cap.dmabuf_fd((uint32_t)i);
            if (fd < 0) {
              if (debug) std::fprintf(stderr, "[rock5b_hdmiin_gl] dmabuf_fd(%zu) invalid\n", i);
              use_zero_copy = false;
              break;
            }

            const int y_w = (int)cap.width();
            const int y_h = (int)cap.height();
            const int uv_w = y_w / 2;
            const int uv_h = y_h / 2;
            const int y_pitch = (int)frame.y_stride;
            const int uv_pitch = (int)frame.uv_stride;
            const int y_offset = 0;
            const int uv_offset = (int)(frame.y_stride * frame.height);

            const EGLint y_attr[] = {
                EGL_WIDTH, y_w,
                EGL_HEIGHT, y_h,
                EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_R8,
                EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, y_offset,
                EGL_DMA_BUF_PLANE0_PITCH_EXT, y_pitch,
                EGL_NONE};
            const EGLint uv_attr[] = {
                EGL_WIDTH, uv_w,
                EGL_HEIGHT, uv_h,
                EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_GR88,
                EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, uv_offset,
                EGL_DMA_BUF_PLANE0_PITCH_EXT, uv_pitch,
                EGL_NONE};

            y_images[i] = eglCreateImageKHR_ptr(gfx.egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)nullptr, y_attr);
            uv_images[i] = eglCreateImageKHR_ptr(gfx.egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)nullptr, uv_attr);
            if (y_images[i] == EGL_NO_IMAGE_KHR || uv_images[i] == EGL_NO_IMAGE_KHR) {
              std::fprintf(stderr, "[rock5b_hdmiin_gl] eglCreateImageKHR failed (err=0x%x)\n", (unsigned)eglGetError());
              use_zero_copy = false;
              break;
            }

            glGenTextures(1, &y_texs[i]);
            glBindTexture(GL_TEXTURE_2D, y_texs[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glEGLImageTargetTexture2DOES_ptr(GL_TEXTURE_2D, (GLeglImageOES)y_images[i]);

            glGenTextures(1, &uv_texs[i]);
            glBindTexture(GL_TEXTURE_2D, uv_texs[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glEGLImageTargetTexture2DOES_ptr(GL_TEXTURE_2D, (GLeglImageOES)uv_images[i]);
          }
        }

        if (use_zero_copy) {
          uint32_t idx = frame.index;
          if (idx < y_texs.size()) {
            cur_y_tex = y_texs[idx];
            cur_uv_tex = uv_texs[idx];

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, cur_y_tex);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, cur_uv_tex);
          }
        }
      }
    } else {
      if (frame.data.empty()) continue;

      glBindTexture(GL_TEXTURE_2D, tex);
      if (!tex_alloc || tex_w != frame.width || tex_h != frame.height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)frame.width, (GLsizei)frame.height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        tex_alloc = true;
        tex_w = frame.width;
        tex_h = frame.height;
      }
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)frame.width, (GLsizei)frame.height, GL_RGB, GL_UNSIGNED_BYTE, frame.data.data());
      cur_rgb_tex = tex;
    }

    if (!two_pass) {
      glClearColor(0.f, 0.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);

      glUseProgram(prog_pre);
      if (use_nv12) {
        glActiveTexture(GL_TEXTURE0);
        glActiveTexture(GL_TEXTURE1);
        glUniform1i(u_tex_y_pre, 0);
        glUniform1i(u_tex_uv_pre, 1);
        glUniform1i(u_uvSwap_pre, nv21 ? 1 : 0);
        if (u_uvRA_pre >= 0) glUniform1i(u_uvRA_pre, dmabuf_uv_ra ? 1 : 0);
      } else {
        glUniform1i(u_tex_pre, 0);
      }

      glEnableVertexAttribArray((GLuint)a_pos_pre);
      glVertexAttribPointer((GLuint)a_pos_pre, 2, GL_FLOAT, GL_FALSE, 0, verts);

      glEnableVertexAttribArray((GLuint)a_uv_pre);
      glVertexAttribPointer((GLuint)a_uv_pre, 2, GL_FLOAT, GL_FALSE, 0, uvs_post);

      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
      const uint32_t src_w = use_nv12 ? frame.width : tex_w;
      const uint32_t src_h = use_nv12 ? frame.height : tex_h;

      if (!fbo_alloc || fbo_w != src_w || fbo_h != src_h) {
        if (fbo == 0) glGenFramebuffers(1, &fbo);
        if (fbo_tex == 0) glGenTextures(1, &fbo_tex);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, fbo_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)src_w, (GLsizei)src_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
          std::fprintf(stderr, "[rock5b_hdmiin_gl] FBO incomplete (0x%x)\n", (unsigned)st);
          return 7;
        }

        fbo_alloc = true;
        fbo_w = src_w;
        fbo_h = src_h;
      }
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glViewport(0, 0, (GLsizei)fbo_w, (GLsizei)fbo_h);
      glClearColor(0.f, 0.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);

      glUseProgram(prog_pre);

      if (use_nv12) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cur_y_tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, cur_uv_tex);
        glUniform1i(u_tex_y_pre, 0);
        glUniform1i(u_tex_uv_pre, 1);
        glUniform1i(u_uvSwap_pre, nv21 ? 1 : 0);
        if (u_uvRA_pre >= 0) glUniform1i(u_uvRA_pre, dmabuf_uv_ra ? 1 : 0);
      } else {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cur_rgb_tex);
        glUniform1i(u_tex_pre, 0);
      }

      glEnableVertexAttribArray((GLuint)a_pos_pre);
      glVertexAttribPointer((GLuint)a_pos_pre, 2, GL_FLOAT, GL_FALSE, 0, verts);
      glEnableVertexAttribArray((GLuint)a_uv_pre);
      glVertexAttribPointer((GLuint)a_uv_pre, 2, GL_FLOAT, GL_FALSE, 0, uvs_pre);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

      if (dbg_early) {
        GLenum e = glGetError();
        std::fprintf(stderr, "[rock5b_hdmiin_gl] dbg stage=prepass glGetError=0x%x\n", (unsigned)e);
        std::fflush(stderr);
      }

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glViewport(0, 0, (GLsizei)gfx.mode_hdisplay, (GLsizei)gfx.mode_vdisplay);
      glClearColor(0.f, 0.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, fbo_tex);
      glUseProgram(prog_post);
      if (u_tex_post >= 0) glUniform1i(u_tex_post, 0);
      if (post_loc_mx >= 0) glUniform1i(post_loc_mx, sub_mx);
      if (post_loc_my >= 0) glUniform1i(post_loc_my, sub_my);
      if (post_loc_views >= 0) glUniform1i(post_loc_views, sub_views);
      if (post_loc_wz >= 0) glUniform1i(post_loc_wz, sub_wz);
      if (post_loc_wn >= 0) glUniform1i(post_loc_wn, sub_wn);
      if (post_loc_test >= 0) glUniform1i(post_loc_test, sub_test);
      if (post_loc_left >= 0) glUniform1i(post_loc_left, sub_left);
      if (post_loc_mstart >= 0) glUniform1i(post_loc_mstart, sub_mstart);
      if (post_loc_hq >= 0) glUniform1i(post_loc_hq, sub_hq);
      if (post_loc_res >= 0) glUniform2i(post_loc_res, (int)gfx.mode_hdisplay, (int)gfx.mode_vdisplay);

      glEnableVertexAttribArray((GLuint)a_pos_post);
      glVertexAttribPointer((GLuint)a_pos_post, 2, GL_FLOAT, GL_FALSE, 0, verts);
      glEnableVertexAttribArray((GLuint)a_uv_post);
      glVertexAttribPointer((GLuint)a_uv_post, 2, GL_FLOAT, GL_FALSE, 0, uvs_post);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

      if (dbg_early) {
        GLenum e = glGetError();
        std::fprintf(stderr, "[rock5b_hdmiin_gl] dbg stage=postpass glGetError=0x%x\n", (unsigned)e);
        std::fflush(stderr);
      }
    }

    if (debug && !first_frame_gl_checked) {
      first_frame_gl_checked = true;
      GLenum err = glGetError();
      if (err != GL_NO_ERROR) {
        std::fprintf(stderr, "[rock5b_hdmiin_gl] GL error after draw: 0x%x\n", (unsigned)err);
      }
    }

    const uint64_t flips_before = gfx.pageflip_submitted;
    if (!drm_gbm_egl_swap_buffers(gfx)) {
      std::fprintf(stderr, "[rock5b_hdmiin_gl] swap_buffers failed\n");
      break;
    }
    const uint64_t flips_after = gfx.pageflip_submitted;

    if (dbg_early) {
      GLenum e = glGetError();
      std::fprintf(stderr, "[rock5b_hdmiin_gl] dbg stage=swap glGetError=0x%x flips_submitted=%llu\n", (unsigned)e, (unsigned long long)flips_after);
      std::fflush(stderr);
    }

    glFlush();

    if (frame.needs_release) {
      if (use_zero_copy) {
        // If DRM pageflip events are not being used (e.g. SetCrtc fallback), pageflip_completed
        // will not advance, so we must release buffers based on successful swaps.
        if (!gfx.pageflip_enabled || !gfx.pageflip_use_event) {
          if (displayed_v4l2_index >= 0) {
            V4L2Frame rel;
            rel.needs_release = true;
            rel.index = (uint32_t)displayed_v4l2_index;
            if (!cap.release_frame(rel)) {
              std::fprintf(stderr, "[rock5b_hdmiin_gl] release_frame failed\n");
              break;
            }
          }
          displayed_v4l2_index = (int)frame.index;
          pending_v4l2_index = -1;
        } else {
          if (flips_after > flips_before) {
            pending_v4l2_index = (int)frame.index;
          } else {
            if (displayed_v4l2_index < 0) {
              displayed_v4l2_index = (int)frame.index;
            } else {
              if (!cap.release_frame(frame)) {
                std::fprintf(stderr, "[rock5b_hdmiin_gl] release_frame failed\n");
                break;
              }
            }
          }
        }
      } else {
        if (!cap.release_frame(frame)) {
          std::fprintf(stderr, "[rock5b_hdmiin_gl] release_frame failed\n");
          break;
        }
      }
    }
  }

  if (use_zero_copy) {
    if (displayed_v4l2_index >= 0) {
      V4L2Frame rel;
      rel.needs_release = true;
      rel.index = (uint32_t)displayed_v4l2_index;
      cap.release_frame(rel);
    }
    if (pending_v4l2_index >= 0) {
      V4L2Frame rel;
      rel.needs_release = true;
      rel.index = (uint32_t)pending_v4l2_index;
      cap.release_frame(rel);
    }
  }

  cap.stop();
  cap.close_device();

  for (size_t i = 0; i < y_images.size(); i++) {
    if (y_images[i] != EGL_NO_IMAGE_KHR && eglDestroyImageKHR_ptr) eglDestroyImageKHR_ptr(gfx.egl_display, y_images[i]);
    if (uv_images[i] != EGL_NO_IMAGE_KHR && eglDestroyImageKHR_ptr) eglDestroyImageKHR_ptr(gfx.egl_display, uv_images[i]);
  }

  destroy_drm_gbm_egl(gfx);
  return 0;
}
