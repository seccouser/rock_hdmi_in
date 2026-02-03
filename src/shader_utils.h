#pragma once

#include <GLES2/gl2.h>

GLuint compile_shader(GLenum type, const char* src);
GLuint link_program(GLuint vs, GLuint fs);
