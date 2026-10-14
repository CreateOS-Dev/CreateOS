// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_MOUSE_H
#define KERNEL_MOUSE_H

#include <stdint.h>

void mouse_init(void);
int mouse_wheel_available(void);
typedef struct {
	int dx;
	int dy;
	int wheel;
	uint8_t buttons;
} mouse_event_t;

int mouse_poll_event(mouse_event_t* event);
int mouse_poll_wheel(int* direction);
void mouse_inject_event(const mouse_event_t* event);

#endif
