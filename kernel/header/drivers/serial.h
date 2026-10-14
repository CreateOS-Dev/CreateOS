// ============================================================
// CreateOS 0.0.1
// ============================================================


#ifndef SERIAL_H
#define SERIAL_H

int serial_init(void);
int serial_is_ready(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_puts_raw(const char *s);
void serial_set_display_mirror(int enabled);
char serial_getc(void);

#endif