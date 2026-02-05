#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
precision highp int;
#else
precision mediump float;
precision mediump int;
#endif
varying vec2 v_uv;
uniform sampler2D u_tex;

uniform int mx;
uniform int my;
uniform int views;
uniform int wz;
uniform int wn;
uniform int test;
uniform int left;
uniform int mstart;
uniform int hq;
uniform int atlas_flip_y;
uniform ivec2 u_resolution;

void main() {
  float inv = 0.0;

  float views_local = max(1.0, float(views));
  float iwz = max(1.0, float(wz));
  float iwn = max(1.0, float(wn));
  float mx_local = max(1.0, float(mx));
  float my_local = max(1.0, float(my));

  float views1 = views_local - 1.0;
  vec2 secpos = floor(gl_FragCoord.xy);
  float yt = secpos.y;
  if (left == 0) yt = float(u_resolution.y) - secpos.y;

  float sr = (secpos.x * 3.0) + ((yt * iwz) / iwn);
  float sr_i = floor(sr);
  vec3 secrgb = vec3(sr_i, sr_i + 1.0, sr_i + 2.0);

  vec3 iwert = mod(secrgb, views_local);
  iwert = iwert + float(mstart);

  float hviews = views_local;

  if (hq == 3) {
    hviews = views_local * iwn;
    views1 = hviews - 1.0;
    float hym = mod(yt, iwn) * iwz;
    float hqwert = mod(hym, iwn);
    hqwert = iwz - hqwert;
    hqwert = abs(hqwert);
    vec3 mtmp = ((views1 - iwert) * iwn) + hqwert;
    iwert = views1 - mod(mtmp, hviews);
  }

  if (inv > 0.0) iwert = vec3(views1) - iwert;
  iwert = iwert + float(mstart);
  iwert = floor(abs(mod(iwert, hviews)));

  vec2 uv = v_uv;

  float mxy = mx_local * my_local;
  vec3 tile = iwert;
  if (mxy < hviews || hq == 2) tile = floor((tile * mxy) / hviews);
  tile = floor(tile);
  // Wrap into [0, mxy)
  tile = mod(tile + mxy, mxy);

  if (test == 13) {
    float denom = max(1.0, mxy - 1.0);
    gl_FragColor = vec4(tile.r / denom, tile.g / denom, tile.b / denom, 1.0);
    return;
  }

  if (test == 14) {
    // Unambiguous sanity check:
    // - White: mx=4,my=4,views=5 (expected for your 4x4 / 5-view setup)
    // - Red: mx mismatch
    // - Blue: my mismatch
    // - Magenta: mx+my mismatch
    // - Yellow: views mismatch
    // - Cyan: views mismatch + mx mismatch
    // - Green: views mismatch + my mismatch
    // - Orange: views mismatch + mx+my mismatch
    bool mx_ok = (mx == 4);
    bool my_ok = (my == 4);
    bool views_ok = (views == 5);

    vec3 c = vec3(1.0);
    if (views_ok) {
      if (mx_ok && my_ok) c = vec3(1.0);
      else if (!mx_ok && !my_ok) c = vec3(1.0, 0.0, 1.0);
      else if (!mx_ok) c = vec3(1.0, 0.0, 0.0);
      else c = vec3(0.0, 0.0, 1.0);
    } else {
      if (mx_ok && my_ok) c = vec3(1.0, 1.0, 0.0);
      else if (!mx_ok && !my_ok) c = vec3(1.0, 0.5, 0.0);
      else if (!mx_ok) c = vec3(0.0, 1.0, 1.0);
      else c = vec3(0.0, 1.0, 0.0);
    }
    gl_FragColor = vec4(c, 1.0);
    return;
  }

  if (test == 15) {
    int ti = int(floor(tile.r + 0.5));
    int mm = max(1, mx * my);
    ti = ti - (ti / mm) * mm;
    if (ti < 0) ti += mm;
    vec3 c = vec3(0.0);
    if (ti == 0) c = vec3(1.0, 0.0, 0.0);
    else if (ti == 1) c = vec3(0.0, 1.0, 0.0);
    else if (ti == 2) c = vec3(0.0, 0.0, 1.0);
    else if (ti == 3) c = vec3(1.0, 1.0, 0.0);
    else if (ti == 4) c = vec3(0.0, 1.0, 1.0);
    else if (ti == 5) c = vec3(1.0, 0.0, 1.0);
    else if (ti == 6) c = vec3(0.0, 0.0, 0.0);
    else if (ti == 7) c = vec3(1.0, 1.0, 1.0);
    else if (ti == 8) c = vec3(1.0, 0.5, 0.0);
    else if (ti == 9) c = vec3(0.0, 0.5, 1.0);
    else if (ti == 10) c = vec3(0.2, 0.8, 0.2);
    else if (ti == 11) c = vec3(0.8, 0.8, 0.2);
    else if (ti == 12) c = vec3(0.6, 0.2, 1.0);
    else if (ti == 13) c = vec3(1.0, 0.2, 0.6);
    else if (ti == 14) c = vec3(0.2, 0.6, 0.2);
    else if (ti == 15) c = vec3(0.7, 0.7, 0.7);
    else {
      float denom = max(1.0, float(mm - 1));
      float f = clamp(float(ti) / denom, 0.0, 1.0);
      c = vec3(f, 1.0 - f, 0.5);
    }
    gl_FragColor = vec4(c, 1.0);
    return;
  }

  float ty_r = floor(tile.r / mx_local);
  float tx_r = tile.r - ty_r * mx_local;
  float ty_g = floor(tile.g / mx_local);
  float tx_g = tile.g - ty_g * mx_local;
  float ty_b = floor(tile.b / mx_local);
  float tx_b = tile.b - ty_b * mx_local;

  if (atlas_flip_y != 0) {
    float my1 = my_local - 1.0;
    ty_r = my1 - ty_r;
    ty_g = my1 - ty_g;
    ty_b = my1 - ty_b;
  }

  if (test == 16) {
    float dx = max(1.0, mx_local - 1.0);
    float dy = max(1.0, my_local - 1.0);
    gl_FragColor = vec4(clamp(tx_r / dx, 0.0, 1.0), clamp(ty_r / dy, 0.0, 1.0), 0.0, 1.0);
    return;
  }

  vec2 uv_r = vec2((tx_r + uv.x) / mx_local, (ty_r + uv.y) / my_local);
  vec2 uv_g = vec2((tx_g + uv.x) / mx_local, (ty_g + uv.y) / my_local);
  vec2 uv_b = vec2((tx_b + uv.x) / mx_local, (ty_b + uv.y) / my_local);

  if (test == 12) {
    vec3 s = texture2D(u_tex, uv_r).rgb;
    gl_FragColor = vec4(s, 1.0);
    return;
  }



  vec3 col = vec3(
      texture2D(u_tex, uv_r).r,
      texture2D(u_tex, uv_g).g,
      texture2D(u_tex, uv_b).b);

  gl_FragColor = vec4(col, 1.0);
}