#include "shader_utils.h"

#include <cstdlib>
#include <cstdio>
#include <vector>

GLuint compile_shader(GLenum type, const char* src) {
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);

  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLint log_len = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &log_len);
    if (log_len > 1) {
      std::vector<char> log((size_t)log_len);
      GLsizei out_len = 0;
      glGetShaderInfoLog(sh, log_len, &out_len, log.data());
      const char* kind = (type == GL_VERTEX_SHADER) ? "VERTEX" : (type == GL_FRAGMENT_SHADER) ? "FRAGMENT" : "UNKNOWN";
      std::fprintf(stderr, "[shader_utils] %s shader compile failed:\n%.*s\n", kind, (int)out_len, log.data());
    } else {
      const char* kind = (type == GL_VERTEX_SHADER) ? "VERTEX" : (type == GL_FRAGMENT_SHADER) ? "FRAGMENT" : "UNKNOWN";
      std::fprintf(stderr, "[shader_utils] %s shader compile failed (no log)\n", kind);
    }
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

GLuint link_program(GLuint vs, GLuint fs) {
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);

  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint log_len = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &log_len);
    if (log_len > 1) {
      std::vector<char> log((size_t)log_len);
      GLsizei out_len = 0;
      glGetProgramInfoLog(prog, log_len, &out_len, log.data());
      std::fprintf(stderr, "[shader_utils] program link failed:\n%.*s\n", (int)out_len, log.data());
    } else {
      std::fprintf(stderr, "[shader_utils] program link failed (no log)\n");
    }
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}
