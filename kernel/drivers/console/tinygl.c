#include "drivers/framebuffer.h"
#include "drivers/tinygl.h"
#include "GL/gl.h"
#include "zbuffer.h"

static ZBuffer* tinygl_buffer;

int framebuffer_tinygl_init(void)
{
    const framebuffer_info_t* framebuffer = framebuffer_info();
    if (tinygl_buffer)
        return 1;
    if (!framebuffer_ready() || !framebuffer ||
        framebuffer->bpp != 32 || framebuffer->width > 0x7FFFFFFF ||
        framebuffer->height > 0x7FFFFFFF)
        return 0;
    tinygl_buffer = ZB_open((int)framebuffer->width,
                            (int)framebuffer->height, ZB_MODE_RGBA,
                            0, 0, 0, 0);
    if (!tinygl_buffer)
        return 0;
    glInit(tinygl_buffer);
    return 1;
}

void framebuffer_tinygl_shutdown(void)
{
    if (!tinygl_buffer)
        return;
    glClose();
    ZB_close(tinygl_buffer);
    tinygl_buffer = 0;
}

int framebuffer_tinygl_present(void)
{
    const framebuffer_info_t* framebuffer = framebuffer_info();
    if (!tinygl_buffer || !framebuffer_ready() || !framebuffer ||
        framebuffer->bpp != 32)
        return 0;
    ZB_copyFrameBuffer(tinygl_buffer, (void*)(uintptr_t)framebuffer->address,
                       (int)framebuffer->pitch);
    return 1;
}
