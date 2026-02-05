# rock_hdmi_in / rock5b-hdmiin-gl

Low-latency HDMI-in passthrough on Rock 5B (and similar Linux SBCs) using:

- V4L2 capture (NV12)
- DRM/KMS + GBM + EGL (direct scanout)
- OpenGL ES 2.0 shaders
- Optional two-pass post-processing (subpixel/mosaic shader)

This project is intended to run **fullscreen on the DRM/KMS display** (no X11/Wayland required).

## Features

- One-pass pipeline: HDMI-in -> shader -> display
- Two-pass pipeline: HDMI-in -> NV12->RGB pre-pass into FBO -> post shader to display
- Zero-copy NV12 path using dmabuf/EGLImage (when supported)
- Shader files are external in `./shaders/`
- Profiles in `./shaders/profiles/*.profile`
- Optional global config file: `~/.config/3dplayer.conf`

## Build prerequisites

### Packages (Debian/Ubuntu/Armbian)

Install build tools and runtime deps:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  libdrm-dev libgbm-dev \
  libegl1-mesa-dev libgles2-mesa-dev \
  libv4l-dev
```

Notes:

- On Rock5B you may be using panfrost/mali userspace; `libegl`/`libgles2` packages can come from Mesa.
- DRM/KMS access usually requires running as root or being in groups like `video` / `render`.

## Build

From the project root:

```bash
mkdir -p build
cmake -S . -B build
cmake --build build -j
```

The binary will be:

- `build/rock5b_hdmiin_gl`

## Run

### Quick start (positional /dev/video0)

```bash
sudo ./build/rock5b_hdmiin_gl /dev/video0
```

### Using a profile (enables subpixel if `subpixel=1` is set in the profile)

```bash
sudo ./build/rock5b_hdmiin_gl /dev/video0 --profile Profile_4x4
```

### Run without positional video device

If you set `video_dev` in the global config (see below), you can run:

```bash
sudo ./build/rock5b_hdmiin_gl --profile Profile_4x4
```

### Explicit device overrides

```bash
sudo ./build/rock5b_hdmiin_gl --video /dev/video0 --drm /dev/dri/card0 --profile Profile_4x4
```

### DRM mode override

By default the DRM/KMS mode is auto-selected from the connected monitor (preferred mode if available).

You can override this with:

- `--mode WxH` (e.g. `1920x1080`)
- `--mode WxH@Hz` (e.g. `1920x1080@60`)
- `--mode <mode_name>` (matches `drmModeModeInfo.name`)

Example:

```bash
sudo ./build/rock5b_hdmiin_gl /dev/video0 --mode 1920x1080@60
```

### Debug logs

```bash
sudo ./build/rock5b_hdmiin_gl /dev/video0 --debug
```

## Notes

- The DRM/KMS output mode is auto-selected from the connected monitor.
  - Preferred mode (DRM flag) is chosen if present.
  - Otherwise the highest resolution (then highest refresh) is selected.
- On some Rockchip HDMI-RX setups, **1080p/FHD input** may negotiate as **NV24** (YUV444) instead of NV12.
  - This project supports NV24 and uses dmabuf/EGLImage zero-copy when available.

## Global config file

Default path:

- `~/.config/3dplayer.conf`

Format:

- `key=value`
- `#` starts comments

Example:

```ini
# Devices
video_dev=/dev/video0
drm_dev=/dev/dri/card0

# Shaders
shader_dir=/home/seccouser/CascadeProjects/rock5b-hdmiin-gl/shaders

# Capture buffers (more buffers can improve stability)
buffers=6

# Default options (0/1)
flip_y=0
subpixel=0
nv21=0
dmabuf_uv_ra=0
```

Override config file:

```bash
sudo ./build/rock5b_hdmiin_gl --config /path/to/3dplayer.conf --profile Profile_4x4
```

Disable config file:

```bash
sudo ./build/rock5b_hdmiin_gl --no-config /dev/video0
```

## Profiles

Profiles live in:

- `shaders/profiles/*.profile`

Example keys:

- Subpixel params: `mx`, `my`, `views`, `wz`, `wn`, `left`, `mstart`, `hq`, `test`
- Boolean options: `flip_y`, `nv21`, `dmabuf_uv_ra`, `subpixel`

Example profile:

```ini
# Profile_4x4.profile
flip_y=0
subpixel=1
mx=4
my=4
views=7
wz=4
wn=5
left=1
mstart=0
hq=0
test=0
```

Select profile:

```bash
sudo ./build/rock5b_hdmiin_gl /dev/video0 --profile Profile_4x4
```

## Troubleshooting

### "Permission denied" opening /dev/video0 or /dev/dri/card0

- Run with `sudo`, or
- Add your user to the right groups (often `video` and/or `render`) and re-login.

### Black screen / no output

- Try `--debug` to see where it stops.
- If your display pipeline is stuck on pageflips, the code includes a timeout and fallback.

### Wrong colors (NV12 UV swizzle)

Some pipelines expose the UV plane differently. Try:

- `--dmabuf-uv-ra` (or `dmabuf_uv_ra=1` in config/profile)
- `--nv21` (or `nv21=1`) if your source is NV21.

### Orientation upside-down

Use:

- `--flip-y` (or `flip_y=1`) to invert orientation.

### Using GitHub (SSH)

If `git push` fails with `Permission denied (publickey)`:

- Ensure your SSH public key is added at GitHub -> Settings -> SSH keys
- Test with:

```bash
ssh -T git@github.com
```

## License

Add a license file if you want to publish this as open source.
