#ifndef KERNEL_TINYGL_H
#define KERNEL_TINYGL_H

#include "../../lib/third_party/tinygl/include/GL/gl.h"

int framebuffer_tinygl_init(void);
void framebuffer_tinygl_shutdown(void);
int framebuffer_tinygl_present(void);

#endif
