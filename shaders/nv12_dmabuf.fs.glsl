precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex_y;
uniform sampler2D u_tex_uv;
uniform int u_uvSwap;
uniform int u_uvRA;
void main(){
  float y = texture2D(u_tex_y, v_uv).r;
  vec4 uv4 = texture2D(u_tex_uv, v_uv);
  vec2 uv = (u_uvRA != 0) ? uv4.ra : uv4.rg;
  float u = (u_uvSwap == 0) ? uv.x : uv.y;
  float v = (u_uvSwap == 0) ? uv.y : uv.x;
  float Y = max(0.0, y * 255.0 - 16.0);
  float U = u * 255.0 - 128.0;
  float V = v * 255.0 - 128.0;
  float r = (298.0*Y + 409.0*V) / 256.0;
  float g = (298.0*Y - 100.0*U - 208.0*V) / 256.0;
  float b = (298.0*Y + 516.0*U) / 256.0;
  gl_FragColor = vec4(r/255.0, g/255.0, b/255.0, 1.0);
}
