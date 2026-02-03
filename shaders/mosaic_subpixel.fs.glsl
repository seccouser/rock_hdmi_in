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
uniform ivec2 u_resolution;

void main() {
  if (test == 2) {
    gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0);
    return;
  }

  if (test == 3) {
    gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);
    return;
  }

  if (test == 4) {
    gl_FragColor = vec4(v_uv.x, v_uv.y, 0.0, 1.0);
    return;
  }

  if (test == 5) {
    gl_FragColor = texture2D(u_tex, v_uv);
    return;
  }

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

  float ty_r = floor(tile.r / mx_local);
  float tx_r = tile.r - ty_r * mx_local;
  float ty_g = floor(tile.g / mx_local);
  float tx_g = tile.g - ty_g * mx_local;
  float ty_b = floor(tile.b / mx_local);
  float tx_b = tile.b - ty_b * mx_local;

  vec2 uv_r = vec2((tx_r + uv.x) / mx_local, (ty_r + uv.y) / my_local);
  vec2 uv_g = vec2((tx_g + uv.x) / mx_local, (ty_g + uv.y) / my_local);
  vec2 uv_b = vec2((tx_b + uv.x) / mx_local, (ty_b + uv.y) / my_local);

  vec3 col = vec3(
      texture2D(u_tex, uv_r).r,
      texture2D(u_tex, uv_g).g,
      texture2D(u_tex, uv_b).b);

  gl_FragColor = vec4(col, 1.0);

  if (test == 1) {
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    if (iwert.r < 0.5) gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
  }
}
