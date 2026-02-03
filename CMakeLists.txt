cmake_minimum_required(VERSION 3.16)
project(rock5b_hdmiin_gl LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)

add_executable(rock5b_hdmiin_gl
  src/main.cpp
  src/drm_gbm_egl.cpp
  src/v4l2_capture.cpp
  src/shader_utils.cpp
)

target_include_directories(rock5b_hdmiin_gl PRIVATE src)

target_include_directories(rock5b_hdmiin_gl PRIVATE
  ${DRM_INCLUDE_DIRS}
  ${GBM_INCLUDE_DIRS}
)

target_compile_options(rock5b_hdmiin_gl PRIVATE
  ${DRM_CFLAGS_OTHER}
  ${GBM_CFLAGS_OTHER}
)

target_link_libraries(rock5b_hdmiin_gl
  ${DRM_LIBRARIES}
  ${GBM_LIBRARIES}
  EGL
  GLESv2
)
