// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "proc/sysconfig.h"
#include "fs/fs.h"
#include "lib/string.h"
#include <stdint.h>


typedef struct {
    char name[32];
    int  enabled;
} cfg_cmd_t;

static cfg_cmd_t g_cfg_cmds[64];
static int       g_cfg_cmd_count = 0;
static int       g_goddo_locked = 0;


static void int_to_str(int v, char* out) {
    char tmp[16];
    int pos = 0;

    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }

    int neg = 0;
    if (v < 0) {
        neg = 1;
        v = -v;
    }

    while (v > 0 && pos < 15) {
        tmp[pos++] = '0' + (v % 10);
        v /= 10;
    }

    int i = 0;
    if (neg) out[i++] = '-';

    while (pos > 0)
        out[i++] = tmp[--pos];

    out[i] = 0;
}

static int write_cmd_line(char* buf, int pos, const char* name, int enabled) {
    int i = pos;

    buf[i++] = 'c';
    buf[i++] = 'm';
    buf[i++] = 'd';
    buf[i++] = ':';

    for (int k = 0; name[k] && i < 1000; k++)
        buf[i++] = name[k];

    buf[i++] = '=';

    char num[16];
    int_to_str(enabled, num);

    for (int k = 0; num[k] && i < 1000; k++)
        buf[i++] = num[k];

    buf[i++] = '\n';
    return i;
}

static int write_goddo_line(char* buf, int pos, int locked) {
    int i = pos;

    const char* key = "goddo_locked=";
    for (int k = 0; key[k] && i < 1000; k++)
        buf[i++] = key[k];

    char num[16];
    int_to_str(locked, num);

    for (int k = 0; num[k] && i < 1000; k++)
        buf[i++] = num[k];

    buf[i++] = '\n';
    return i;
}


int sysconfig_cmd_get(const char* name) {
    for (int i = 0; i < g_cfg_cmd_count; i++) {
        if (!strcmp(g_cfg_cmds[i].name, name))
            return g_cfg_cmds[i].enabled;
    }
    return 1;
}

void sysconfig_cmd_set(const char* name, int enabled) {
    for (int i = 0; i < g_cfg_cmd_count; i++) {
        if (!strcmp(g_cfg_cmds[i].name, name)) {
            g_cfg_cmds[i].enabled = enabled;
            return;
        }
    }
    if (g_cfg_cmd_count < 64) {
        strncpy(g_cfg_cmds[g_cfg_cmd_count].name, name, 31);
        g_cfg_cmds[g_cfg_cmd_count].name[31] = 0;
        g_cfg_cmds[g_cfg_cmd_count].enabled = enabled;
        g_cfg_cmd_count++;
    }
}

int sysconfig_goddo_locked(void) {
    return g_goddo_locked;
}

void sysconfig_set_goddo_locked(int locked) {
    g_goddo_locked = locked;
}

/* format /etc/sysconfig :
   cmd:ls=1
   cmd:rm=1
   cmd:goddo=1
   goddo_locked=0
*/

int sysconfig_load(void) {
    char buf[1024];
    uint32_t sz = 0;

    if (!fs_load_file("/etc/sysconfig", (uint8_t*)buf, sizeof(buf) - 1, &sz))
        return 0;

    buf[sz] = 0;

    g_cfg_cmd_count = 0;
    g_goddo_locked = 0;

    char* p = buf;
    while (*p) {
        char* line = p;

        while (*p && *p != '\n')
            p++;

        if (*p == '\n') *p++ = 0;

        if (!line[0]) continue;

        if (!strncmp(line, "cmd:", 4)) {
            char* name = line + 4;
            char* eq = strchr(name, '=');
            if (!eq) continue;
            *eq = 0;

            int enabled = atoi(eq + 1);
            sysconfig_cmd_set(name, enabled);
        }
        else if (!strncmp(line, "goddo_locked=", 13)) {
            g_goddo_locked = atoi(line + 13);
        }
    }

    return 1;
}

int sysconfig_save(void) {
    char buf[1024];
    int pos = 0;

    for (int i = 0; i < g_cfg_cmd_count; i++) {
        pos = write_cmd_line(buf, pos, g_cfg_cmds[i].name, g_cfg_cmds[i].enabled);
        if (pos >= 1000) break;
    }

    pos = write_goddo_line(buf, pos, g_goddo_locked);

    return fs_write_file("/etc/sysconfig", (const uint8_t*)buf, pos);
}
