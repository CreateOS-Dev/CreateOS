// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/framebuffer.h"
#include "fonts/font8x8.h"
#include "fonts/font5x7.h"
#include "fonts/noto_sans.h"
#include "fonts/schrift.h"
#include "memory/vmm.h"
#include "lib/string.h"

static framebuffer_info_t info;
static bitmap_font_t noto_sans_font = {
    "Noto Sans", 10, 20, 0, 0, 0, 0, 0, 0, 0
};
static const bitmap_font_t* fonts[] = {
    &font8x8, &font5x7, &noto_sans_font
};
#define FRAMEBUFFER_CONSOLES 5
#define FRAMEBUFFER_MAX_COLUMNS 512
#define FRAMEBUFFER_SCROLLBACK_LINES 512
#define POINTER_SCALE 1
#define POINTER_MAX_SIZE (16 * POINTER_SCALE)

typedef struct {
    uint32_t character;
    uint8_t color;
} framebuffer_cell_t;

typedef struct {
    framebuffer_cell_t cells[FRAMEBUFFER_SCROLLBACK_LINES]
                            [FRAMEBUFFER_MAX_COLUMNS];
    uint64_t cursor_line;
    uint64_t text_cursor_origin;
    uint32_t cursor_column;
    uint32_t scroll_offset;
    uint8_t text_cursor_origin_valid;
} framebuffer_console_t;

static framebuffer_console_t consoles[FRAMEBUFFER_CONSOLES];
static int active_console;
static int selected_font;
static int mapped;
static int font_scale = 1;
static SFT_Font* noto_sans;
static SFT noto_sans_context;
static uint8_t utf8_remaining[FRAMEBUFFER_CONSOLES];
static uint32_t utf8_codepoint[FRAMEBUFFER_CONSOLES];
static uint32_t utf8_minimum[FRAMEBUFFER_CONSOLES];

int framebuffer_font_init(void)
{
    if (noto_sans) return 1;
    noto_sans = sft_loadmem(createos_noto_sans_ttf_start,
                            createos_noto_sans_ttf_size());
    if (!noto_sans) return 0;
    noto_sans_context.font = noto_sans;
    noto_sans_context.xScale = 10.0;
    noto_sans_context.yScale = 16.0;
    noto_sans_context.xOffset = 0.0;
    noto_sans_context.yOffset = 0.0;
    noto_sans_context.flags = SFT_DOWNWARD_Y;
    return 1;
}

static uint32_t color_from_vga(uint8_t color)
{
    static const uint32_t palette[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
        0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
    };
    return palette[color & 0x0F];
}
static int pointer_drawn;
static int pointer_cell_x;
static int pointer_cell_y;
static int pointer_pixel_x;
static int pointer_pixel_y;
static uint8_t pointer_width = 8;
static uint8_t pointer_sprite_height = 8;
static uint8_t pointer_hotspot_x;
static uint8_t pointer_hotspot_y;
static uint8_t pointer_draw_width = 8;
static uint8_t pointer_draw_height = 8;
static int pointer_busy;
static uint8_t pointer_animation_phase;
static uint64_t pointer_animation_tick;
static uint16_t pointer_rows[16] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xDC, 0x8E, 0x07
};
static uint16_t pointer_outline_rows[16];
static int pointer_has_explicit_outline;
static uint32_t pointer_color = 0xFFFFFF;
static uint32_t pointer_outline = 0x101820;
static uint32_t pointer_saved[POINTER_MAX_SIZE * POINTER_MAX_SIZE];
static const char* const pointer_busy_frames[4][16] = {
    {
        "1200000002120000", "1120000020002000",
        "1112000200000200", "1111200200000200",
        "1111120020002000", "1111112002220000",
        "1111111200000000", "1111111120000000",
        "1111112222200000", "1111212000000000",
        "1112011200000000", "1120011200000000",
        "1200001120000000", "2000001120000000",
        "0000002220000000", "0000000000000000"
    },
    {
        "1200000002220000", "1120000020001000",
        "1112000200000220", "1111200200000200",
        "1111120020002000", "1111112002220000",
        "1111111200000000", "1111111120000000",
        "1111112222200000", "1111212000000000",
        "1112011200000000", "1120011200000000",
        "1200001120000000", "2000001120000000",
        "0000002220000000", "0000000000000000"
    },
    {
        "1200000002220000", "1120000020002000",
        "1112000200000200", "1111200200000200",
        "1111120020002000", "1111112002120000",
        "1111111200000000", "1111111120000000",
        "1111112222200000", "1111212000000000",
        "1112011200000000", "1120011200000000",
        "1200001120000000", "2000001120000000",
        "0000002220000000", "0000000000000000"
    },
    {
        "1200000002220000", "1120000010002000",
        "1112002200000200", "1111200200000200",
        "1111120020002000", "1111112002220000",
        "1111111200000000", "1111111120000000",
        "1111112222200000", "1111212000000000",
        "1112011200000000", "1120011200000000",
        "1200001120000000", "2000001120000000",
        "0000002220000000", "0000000000000000"
    }
};

static uint32_t framebuffer_read_pixel(uint32_t x, uint32_t y)
{
    volatile uint32_t* pixel = (volatile uint32_t*)
        ((volatile uint8_t*)(uintptr_t)info.address + y * info.pitch +
         x * sizeof(uint32_t));
    return *pixel;
}

static void framebuffer_pointer_restore(void)
{
    uint32_t draw_width = (uint32_t)pointer_draw_width * POINTER_SCALE;
    uint32_t draw_height = (uint32_t)pointer_draw_height * POINTER_SCALE;
    if (!pointer_drawn || info.bpp != 32) return;
    for (uint32_t y = 0; y < draw_height; y++) {
        for (uint32_t x = 0; x < draw_width; x++) {
            int screen_x = pointer_pixel_x + (int)x;
            int screen_y = pointer_pixel_y + (int)y;
            if (screen_x >= 0 && screen_y >= 0 &&
                (uint32_t)screen_x < info.width &&
                (uint32_t)screen_y < info.height)
                framebuffer_put_pixel((uint32_t)screen_x,
                                      (uint32_t)screen_y,
                                      pointer_saved[y * draw_width + x]);
        }
    }
    pointer_drawn = 0;
}

static int cursor_parse_uint(const char* text, uint32_t* value)
{
    uint32_t base = 10;
    uint32_t result = 0;
    if (!text || !*text || !value) return 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
        base = 16;
    }
    if (!*text) return 0;
    while (*text) {
        uint32_t digit;
        if (*text >= '0' && *text <= '9') digit = (uint32_t)(*text - '0');
        else if (base == 16 && *text >= 'a' && *text <= 'f')
            digit = (uint32_t)(*text - 'a' + 10);
        else if (base == 16 && *text >= 'A' && *text <= 'F')
            digit = (uint32_t)(*text - 'A' + 10);
        else return 0;
        if (digit >= base || result > (UINT32_MAX - digit) / base) return 0;
        result = result * base + digit;
        text++;
    }
    *value = result;
    return 1;
}

static char* cursor_skip_space(char* text)
{
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static char* cursor_next_token(char** text)
{
    char* token = cursor_skip_space(*text);
    char* end = token;
    while (*end && *end != ' ' && *end != '\t') end++;
    if (*end) *end++ = 0;
    *text = end;
    return token;
}

int framebuffer_set_pointer_sprite(const char* data, uint32_t size)
{
    uint16_t rows[16] = {0};
    uint16_t outline_rows[16] = {0};
    uint8_t width = 0, height = 0, hotspot_x = 0, hotspot_y = 0;
    uint32_t color = 0, outline = 0;
    uint32_t parsed_rows = 0;
    int has_explicit_outline = 0;
    int have_header = 0, have_color = 0, in_pixels = 0;
    int redraw_pointer = pointer_drawn;
    int pointer_x = pointer_cell_x;
    int pointer_y = pointer_cell_y;
    char line[64];
    uint32_t offset = 0;

    if (!data || !size || size > 4096) return 0;
    while (offset < size) {
        uint32_t length = 0;
        while (offset < size && data[offset] != '\n' &&
               length < sizeof(line) - 1) {
            if (data[offset] != '\r') line[length++] = data[offset];
            offset++;
        }
        if (offset < size && data[offset] == '\n') offset++;
        if (offset < size && length == sizeof(line) - 1 &&
            data[offset - 1] != '\n')
            return 0;
        line[length] = 0;
        if (in_pixels) {
            if (!length) continue;
            if (parsed_rows >= height || length != width) return 0;
            for (uint32_t x = 0; x < width; x++) {
                if (line[x] != '0' && line[x] != '1' && line[x] != '2')
                    return 0;
                if (line[x] == '1')
                    rows[parsed_rows] |= (uint16_t)(1U << (width - x - 1));
                else if (line[x] == '2') {
                    outline_rows[parsed_rows] |=
                        (uint16_t)(1U << (width - x - 1));
                    has_explicit_outline = 1;
                }
            }
            parsed_rows++;
            continue;
        }

        char* cursor = line;
        char* key = cursor_next_token(&cursor);
        char* value = cursor_next_token(&cursor);
        if (!*key) continue;
        if (strcmp(key, "SRBMP") == 0) {
            if (strcmp(value, "1") != 0) return 0;
            have_header = 1;
        } else if (strcmp(key, "width") == 0) {
            uint32_t number;
            if (!cursor_parse_uint(value, &number) || !number || number > 16)
                return 0;
            width = (uint8_t)number;
        } else if (strcmp(key, "height") == 0) {
            uint32_t number;
            if (!cursor_parse_uint(value, &number) || !number || number > 16)
                return 0;
            height = (uint8_t)number;
        } else if (strcmp(key, "hotspot") == 0) {
            uint32_t x, y;
            char* second = cursor_next_token(&cursor);
            if (!cursor_parse_uint(value, &x) ||
                !cursor_parse_uint(second, &y) || x > 15 || y > 15)
                return 0;
            hotspot_x = (uint8_t)x;
            hotspot_y = (uint8_t)y;
        } else if (strcmp(key, "color") == 0) {
            if (!cursor_parse_uint(value, &color)) return 0;
            color &= 0xFFFFFF;
            have_color = 1;
        } else if (strcmp(key, "outline") == 0) {
            if (!cursor_parse_uint(value, &outline)) return 0;
            outline &= 0xFFFFFF;
        } else if (strcmp(key, "pixels") == 0) {
            if (!width || !height || !have_color) return 0;
            in_pixels = 1;
        }
    }
    if (!have_header || !in_pixels || parsed_rows != height ||
        hotspot_x >= width || hotspot_y >= height)
        return 0;

    framebuffer_pointer_restore();
    memcpy(pointer_rows, rows, sizeof(pointer_rows));
    memcpy(pointer_outline_rows, outline_rows, sizeof(pointer_outline_rows));
    pointer_has_explicit_outline = has_explicit_outline;
    pointer_width = width;
    pointer_sprite_height = height;
    pointer_hotspot_x = hotspot_x;
    pointer_hotspot_y = hotspot_y;
    pointer_color = color;
    pointer_outline = outline;
    if (redraw_pointer) framebuffer_draw_pointer(pointer_x, pointer_y);
    return 1;
}

void framebuffer_set_pointer_busy(int busy)
{
    int redraw_pointer = pointer_drawn;
    int pointer_x = pointer_cell_x;
    int pointer_y = pointer_cell_y;
    framebuffer_pointer_restore();
    pointer_busy = busy != 0;
    pointer_animation_phase = 0;
    pointer_animation_tick = 0;
    if (redraw_pointer)
        framebuffer_draw_pointer(pointer_x, pointer_y);
}

void framebuffer_pointer_tick(uint64_t ticks)
{
    uint8_t phase;
    int pointer_x;
    int pointer_y;
    if (!pointer_busy || !pointer_drawn || ticks - pointer_animation_tick < 8)
        return;
    phase = (uint8_t)((ticks / 8) & 3);
    if (phase == pointer_animation_phase)
        return;
    pointer_animation_tick = ticks;
    pointer_animation_phase = phase;
    pointer_x = pointer_cell_x;
    pointer_y = pointer_cell_y;
    framebuffer_pointer_restore();
    framebuffer_draw_pointer(pointer_x, pointer_y);
}

static framebuffer_console_t* current_console(void)
{
    return &consoles[active_console];
}

static uint32_t framebuffer_columns(const bitmap_font_t* font)
{
    uint32_t columns = info.width / (font->width * (uint32_t)font_scale);
    return columns > FRAMEBUFFER_MAX_COLUMNS ?
           FRAMEBUFFER_MAX_COLUMNS : columns;
}

static uint32_t framebuffer_rows(const bitmap_font_t* font)
{
    uint32_t rows = info.height / (font->height * (uint32_t)font_scale);
    return rows > FRAMEBUFFER_SCROLLBACK_LINES ?
           FRAMEBUFFER_SCROLLBACK_LINES : rows;
}

static framebuffer_cell_t* console_line(framebuffer_console_t* console,
                                        uint64_t line)
{
    return console->cells[line % FRAMEBUFFER_SCROLLBACK_LINES];
}

static void clear_console_line(framebuffer_console_t* console, uint64_t line)
{
    framebuffer_cell_t* cells = console_line(console, line);
    for (uint32_t column = 0; column < FRAMEBUFFER_MAX_COLUMNS; column++) {
        cells[column].character = ' ';
        cells[column].color = 0x07;
    }
}

static void framebuffer_draw_glyph(uint32_t column, uint32_t row,
                                   uint32_t character, uint8_t color)
{
    const bitmap_font_t* cell_font = fonts[selected_font];
    const bitmap_font_t* font = cell_font;
    uint32_t foreground = color_from_vga(color);
    uint32_t background = color_from_vga(color >> 4);
    uint32_t cell_width = cell_font->width * (uint32_t)font_scale;
    uint32_t cell_height = cell_font->height * (uint32_t)font_scale;
    uint32_t cell_x = column * cell_width;
    uint32_t cell_y = row * cell_height;
    const uint8_t* bitmap;

    for (uint32_t y = 0; y < cell_height; y++)
        for (uint32_t x = 0; x < cell_width; x++)
            framebuffer_put_pixel(cell_x + x, cell_y + y, background);

    if (selected_font == 2 && noto_sans && character != ' ' &&
        character != '\0') {
        SFT_Glyph glyph;
        SFT_GMetrics metrics;
        uint8_t pixels[32 * 32];
        SFT_Image image = { pixels, 0, 0 };
        noto_sans_context.xScale = 10.0;
        noto_sans_context.yScale = 16.0;
        if (sft_lookup(&noto_sans_context, character, &glyph) == 0 &&
            sft_gmetrics(&noto_sans_context, glyph, &metrics) == 0 &&
            metrics.minWidth > 0 && metrics.minHeight > 0 &&
            metrics.minWidth <= 32 && metrics.minHeight <= 32) {
            image.width = metrics.minWidth;
            image.height = metrics.minHeight;
            memset(pixels, 0, sizeof(pixels));
            if (sft_render(&noto_sans_context, glyph, image) == 0) {
                int origin_x = (int)cell_x +
                    (int)metrics.leftSideBearing * font_scale;
                int baseline_y = (int)cell_y +
                    (int)(font->height - 1) * font_scale;
                int origin_y = baseline_y + metrics.yOffset * font_scale;
                for (int y = 0; y < image.height; y++) {
                    for (int x = 0; x < image.width; x++) {
                        uint8_t alpha = pixels[y * image.width + x];
                        uint32_t blended =
                            ((((foreground >> 16) & 0xFF) * alpha +
                              ((background >> 16) & 0xFF) * (255 - alpha)) /
                             255) << 16 |
                            ((((foreground >> 8) & 0xFF) * alpha +
                              ((background >> 8) & 0xFF) * (255 - alpha)) /
                             255) << 8 |
                            (((foreground & 0xFF) * alpha +
                              (background & 0xFF) * (255 - alpha)) / 255);
                        for (int dy = 0; dy < font_scale; dy++) {
                            for (int dx = 0; dx < font_scale; dx++) {
                                int px = origin_x + x * font_scale + dx;
                                int py = origin_y + y * font_scale + dy;
                                if (px >= (int)cell_x &&
                                    px < (int)(cell_x +
                                        font->width * font_scale) &&
                                    py >= (int)cell_y &&
                                    py < (int)(cell_y +
                                        font->height * font_scale))
                                    framebuffer_put_pixel((uint32_t)px,
                                        (uint32_t)py, blended);
                            }
                        }
                    }
                }
                return;
            }
        }
        character = '?';
        font = &font8x8;
    } else if (selected_font == 2) {
        font = &font8x8;
    }
    if (!font->glyphs) return;
    if (character > 0x7F) character = '?';
    uint8_t bitmap_character = (uint8_t)character;
    if (bitmap_character && bitmap_character != ' ' &&
        bitmap_character >= 'a' && bitmap_character <= 'z' &&
        font->lowercase_glyphs) {
        bitmap = &font->lowercase_glyphs[(bitmap_character - 'a') * 8];
    } else if (bitmap_character >= 128 && font->extended_glyphs &&
               bitmap_character >= font->extended_first &&
               bitmap_character <= font->extended_last) {
        bitmap = &font->extended_glyphs[
            (bitmap_character - font->extended_first) * font->height];
    } else if (bitmap_character && bitmap_character != ' ') {
        if (bitmap_character < font->first || bitmap_character > font->last)
            bitmap_character = '?';
        bitmap = &font->glyphs[
            (bitmap_character - font->first) * font->height];
    } else {
        bitmap = 0;
    }

    if (info.bpp == 32) {
        uint32_t pixel_x = cell_x;
        uint32_t pixel_y = cell_y;
        volatile uint8_t* base =
            (volatile uint8_t*)(uintptr_t)info.address;
        for (uint32_t glyph_row = 0; glyph_row < font->height; glyph_row++) {
            uint8_t bits = bitmap ? bitmap[glyph_row] : 0;
            for (uint32_t y = 0; y < cell_height; y++) {
                if (y * font->height / cell_height != glyph_row) continue;
                volatile uint32_t* pixels = (volatile uint32_t*)
                    (base + (pixel_y + y) * info.pitch +
                     pixel_x * sizeof(uint32_t));
                for (uint32_t x = 0; x < cell_width; x++) {
                    uint32_t glyph_column = x * font->width / cell_width;
                    if (bits & (1U << (7 - glyph_column)))
                        pixels[x] = foreground;
                }
            }
        }
        return;
    }

    for (uint32_t y = 0; y < cell_height; y++) {
        uint32_t glyph_row = y * font->height / cell_height;
        uint8_t bits = bitmap ? bitmap[glyph_row] : 0;
        for (uint32_t x = 0; x < cell_width; x++) {
            uint32_t glyph_column = x * font->width / cell_width;
            if (bits & (1U << (7 - glyph_column)))
                framebuffer_put_pixel(cell_x + x, cell_y + y, foreground);
        }
    }
}

static void framebuffer_draw_line(framebuffer_console_t* console,
                                  uint64_t line, uint32_t row)
{
    const bitmap_font_t* font = fonts[selected_font];
    uint32_t columns = framebuffer_columns(font);
    framebuffer_cell_t* cells = console_line(console, line);
    for (uint32_t column = 0; column < columns; column++) {
        framebuffer_draw_glyph(column, row, cells[column].character,
                               cells[column].color);
    }
}

static void framebuffer_redraw_console(void)
{
    framebuffer_console_t* console = current_console();
    const bitmap_font_t* font = fonts[selected_font];
    uint32_t rows;
    uint64_t latest_start;
    uint64_t oldest_start;
    uint64_t start;
    uint32_t max_offset;
    int redraw_pointer;
    int pointer_x;
    int pointer_y;

    if (!framebuffer_ready() || !font) return;
    rows = framebuffer_rows(font);
    if (!framebuffer_columns(font) || !rows) return;

    latest_start = console->cursor_line >= rows - 1 ?
                   console->cursor_line - (rows - 1) : 0;
    oldest_start = console->cursor_line >= FRAMEBUFFER_SCROLLBACK_LINES - 1 ?
                   console->cursor_line - (FRAMEBUFFER_SCROLLBACK_LINES - 1) : 0;
    max_offset = latest_start > oldest_start ?
                 (uint32_t)(latest_start - oldest_start) : 0;
    if (console->scroll_offset > max_offset)
        console->scroll_offset = max_offset;
    start = latest_start - console->scroll_offset;

    redraw_pointer = pointer_drawn;
    pointer_x = pointer_cell_x;
    pointer_y = pointer_cell_y;
    framebuffer_pointer_restore();
    framebuffer_fill(0);
    for (uint32_t row = 0; row < rows; row++) {
        uint64_t line_number = start + row;
        framebuffer_draw_line(console, line_number, row);
    }
    if (redraw_pointer) framebuffer_draw_pointer(pointer_x, pointer_y);
}

static void framebuffer_advance_line(framebuffer_console_t* console,
                                     uint32_t rows)
{
    const bitmap_font_t* font = fonts[selected_font];
    uint32_t line_height = font->height * (uint32_t)font_scale;
    console->cursor_column = 0;
    console->cursor_line++;
    clear_console_line(console, console->cursor_line);

    if (console->scroll_offset) {
        console->scroll_offset = 0;
        framebuffer_redraw_console();
    } else if (console->cursor_line >= rows) {
        volatile uint8_t* base =
            (volatile uint8_t*)(uintptr_t)info.address;
        uint64_t move_bytes = (uint64_t)(info.height - line_height) *
                              info.pitch;
        uint64_t clear_bytes = (uint64_t)line_height * info.pitch;
        volatile uint8_t* clear_start = base + move_bytes;

        if (!((uintptr_t)base & 7) && !(info.pitch & 7)) {
            volatile uint64_t* destination = (volatile uint64_t*)base;
            volatile uint64_t* source =
                (volatile uint64_t*)(base + clear_bytes);
            uint64_t words = move_bytes / sizeof(uint64_t);
            for (uint64_t i = 0; i < words; i++)
                destination[i] = source[i];
            volatile uint64_t* cleared = (volatile uint64_t*)clear_start;
            words = clear_bytes / sizeof(uint64_t);
            for (uint64_t i = 0; i < words; i++)
                cleared[i] = 0;
        } else {
            for (uint64_t i = 0; i < move_bytes; i++)
                base[i] = base[i + clear_bytes];
            for (uint64_t i = 0; i < clear_bytes; i++)
                clear_start[i] = 0;
        }
        framebuffer_draw_line(console, console->cursor_line, rows - 1);
    } else {
        framebuffer_draw_line(console, console->cursor_line,
                              (uint32_t)console->cursor_line);
    }
}

void framebuffer_init(const void* multiboot_info)
{
    const uint8_t* base = (const uint8_t*)multiboot_info;
    selected_font = 0;
    mapped = 0;
    active_console = 0;
    pointer_drawn = 0;
    if (!base) return;

    uint32_t total = *(const uint32_t*)base;
    const uint8_t* tag = base + 8;
    while ((uint32_t)(tag - base) + 8 <= total) {
        uint32_t type = *(const uint32_t*)tag;
        uint32_t size = *(const uint32_t*)(tag + 4);
        if (type == 0 || size < 8) break;
        if (type == 8 && size >= 32) {
            info.address = *(const uint64_t*)(tag + 8);
            info.pitch = *(const uint32_t*)(tag + 16);
            info.width = *(const uint32_t*)(tag + 20);
            info.height = *(const uint32_t*)(tag + 24);
            info.bpp = *(const uint8_t*)(tag + 28);
            info.type = *(const uint8_t*)(tag + 29);
            info.available = info.address != 0 && info.width != 0 && info.height != 0 &&
                             info.type != 2;
            return;
        }
        tag += (size + 7) & ~7U;
    }
}

int framebuffer_map(void)
{
    uint64_t start;
    uint64_t end;
    if (!info.available) return 0;
    if (info.bpp != 15 && info.bpp != 16 && info.bpp != 24 && info.bpp != 32)
        return 0;
    start = info.address & ~0xFFFULL;
    end = (info.address + (uint64_t)info.pitch * info.height + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t address = start; address < end; address += 0x1000) {
        if (!vmm_map_frame(address, PAGE_RW)) return 0;
    }
    mapped = 1;
    return 1;
}

int framebuffer_ready(void) { return info.available && mapped; }

const framebuffer_info_t* framebuffer_info(void)
{
    return &info;
}

void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    volatile uint8_t* pixel;
    if (!info.available || !mapped || x >= info.width || y >= info.height ||
        (info.bpp != 15 && info.bpp != 16 && info.bpp != 24 && info.bpp != 32))
        return;
    pixel = (volatile uint8_t*)(uintptr_t)info.address + y * info.pitch +
            x * ((info.bpp + 7) / 8);
    if (info.bpp == 32) {
        *(volatile uint32_t*)pixel = color;
    } else if (info.bpp == 15 || info.bpp == 16) {
        uint16_t red = (uint16_t)((color >> 19) & 0x1F);
        uint16_t green = (uint16_t)((color >> (info.bpp == 15 ? 11 : 10)) &
                                    (info.bpp == 15 ? 0x1F : 0x3F));
        uint16_t blue = (uint16_t)((color >> 3) & 0x1F);
        *(volatile uint16_t*)pixel = info.bpp == 15 ?
            (uint16_t)((red << 10) | (green << 5) | blue) :
            (uint16_t)((red << 11) | (green << 5) | blue);
    } else {
        pixel[0] = (uint8_t)color;
        pixel[1] = (uint8_t)(color >> 8);
        pixel[2] = (uint8_t)(color >> 16);
    }
}

void framebuffer_fill(uint32_t color)
{
    if (!info.available || (info.bpp != 15 && info.bpp != 16 &&
                            info.bpp != 24 && info.bpp != 32)) return;
    for (uint32_t y = 0; y < info.height; y++)
        for (uint32_t x = 0; x < info.width; x++)
            framebuffer_put_pixel(x, y, color);
}

void framebuffer_clear(void)
{
    framebuffer_console_t* console = current_console();
    int redraw_pointer = pointer_drawn;
    int pointer_x = pointer_cell_x;
    int pointer_y = pointer_cell_y;
    framebuffer_pointer_restore();
    for (uint32_t line = 0; line < FRAMEBUFFER_SCROLLBACK_LINES; line++)
        clear_console_line(console, line);
    console->cursor_line = 0;
    console->text_cursor_origin = 0;
    console->cursor_column = 0;
    console->scroll_offset = 0;
    console->text_cursor_origin_valid = 0;
    framebuffer_fill(0);
    if (redraw_pointer) framebuffer_draw_pointer(pointer_x, pointer_y);
}

void framebuffer_scroll(int lines)
{
    framebuffer_console_t* console = current_console();
    const bitmap_font_t* font = fonts[selected_font];
    uint32_t rows;
    uint64_t latest_start;
    uint64_t oldest_start;
    uint32_t max_offset;
    int64_t offset;
    uint64_t old_start;
    uint64_t new_start;
    uint64_t line_height;
    uint64_t area_height;
    uint64_t shift_bytes;
    uint64_t move_bytes;
    volatile uint8_t* pixels;
    int restore_pointer;
    int pointer_x;
    int pointer_y;

    if (!font || !framebuffer_ready()) return;
    rows = framebuffer_rows(font);
    if (!rows) return;
    latest_start = console->cursor_line >= rows - 1 ?
                   console->cursor_line - (rows - 1) : 0;
    oldest_start = console->cursor_line >= FRAMEBUFFER_SCROLLBACK_LINES - 1 ?
                   console->cursor_line - (FRAMEBUFFER_SCROLLBACK_LINES - 1) : 0;
    max_offset = latest_start > oldest_start ?
                 (uint32_t)(latest_start - oldest_start) : 0;
    old_start = latest_start - console->scroll_offset;
    offset = (int64_t)console->scroll_offset + lines;
    if (offset < 0) offset = 0;
    if ((uint64_t)offset > max_offset) offset = max_offset;
    if ((uint32_t)offset == console->scroll_offset) return;
    console->scroll_offset = (uint32_t)offset;
    new_start = latest_start - console->scroll_offset;

    restore_pointer = pointer_drawn;
    pointer_x = pointer_cell_x;
    pointer_y = pointer_cell_y;
    framebuffer_pointer_restore();
    line_height = (uint64_t)font->height * (uint32_t)font_scale;
    area_height = (uint64_t)rows * line_height;
    if (!line_height || area_height > info.height ||
        (old_start > new_start ? old_start - new_start :
                                 new_start - old_start) >= rows) {
        framebuffer_redraw_console();
    } else {
        uint64_t changed_lines = old_start > new_start ?
                                 old_start - new_start :
                                 new_start - old_start;
        uint32_t changed_rows = (uint32_t)changed_lines;
        uint64_t changed_pixels = changed_lines * line_height;
        int moving_down = new_start < old_start;
        pixels = (volatile uint8_t*)(uintptr_t)info.address;
        shift_bytes = changed_pixels * info.pitch;
        move_bytes = (area_height - changed_pixels) * info.pitch;

        if (moving_down) {
            for (uint64_t i = move_bytes; i > 0; i--)
                pixels[i - 1 + shift_bytes] = pixels[i - 1];
            for (uint64_t i = 0; i < shift_bytes; i++) pixels[i] = 0;
            for (uint32_t row = 0; row < changed_rows; row++)
                framebuffer_draw_line(console, new_start + row, row);
        } else {
            for (uint64_t i = 0; i < move_bytes; i++)
                pixels[i] = pixels[i + shift_bytes];
            for (uint64_t i = area_height * info.pitch - shift_bytes;
                 i < area_height * info.pitch; i++)
                pixels[i] = 0;
            for (uint32_t row = rows - changed_rows; row < rows; row++)
                framebuffer_draw_line(console,
                                      new_start + row, row);
        }
    }
    if (restore_pointer) framebuffer_draw_pointer(pointer_x, pointer_y);
}

void framebuffer_scroll_page(int direction)
{
    uint32_t rows;
    if (!direction) return;
    rows = framebuffer_rows(fonts[selected_font]);
    if (rows <= 2) {
        framebuffer_scroll(direction);
        return;
    }
    framebuffer_scroll(direction > 0 ? (int)(rows - 2) :
                                       -(int)(rows - 2));
}

void framebuffer_select_console(int console)
{
    if (console < 0 || console >= FRAMEBUFFER_CONSOLES) return;
    active_console = console;
    framebuffer_redraw_console();
}

int framebuffer_set_text_cursor(uint32_t column, uint32_t row)
{
    framebuffer_console_t* console = current_console();
    const bitmap_font_t* font = fonts[selected_font];
    uint32_t columns;
    uint32_t rows;
    uint64_t visible_start;

    if (!framebuffer_ready() || !font) return 0;
    columns = framebuffer_columns(font);
    rows = framebuffer_rows(font);
    if (column >= columns || row >= rows) return 0;

    if (!console->text_cursor_origin_valid) {
        visible_start = console->cursor_line >= rows - 1 ?
                        console->cursor_line - (rows - 1) : 0;
        console->text_cursor_origin = visible_start;
        console->text_cursor_origin_valid = 1;
    }
    console->cursor_line = console->text_cursor_origin + row;
    console->cursor_column = column;
    console->scroll_offset = 0;
    return 1;
}

void framebuffer_text_dimensions(uint32_t* columns, uint32_t* rows)
{
    const bitmap_font_t* font = fonts[selected_font];
    if (columns) *columns = 0;
    if (rows) *rows = 0;
    if (!framebuffer_ready() || !font) return;
    if (columns) *columns = framebuffer_columns(font);
    if (rows) *rows = framebuffer_rows(font);
}

int framebuffer_font_count(void)
{
    return noto_sans ? (int)(sizeof(fonts) / sizeof(fonts[0])) : 2;
}

const char* framebuffer_font_name(int index)
{
    if (index < 0 || index >= framebuffer_font_count()) return "none";
    return fonts[index]->name;
}

int framebuffer_font(void) { return selected_font; }

int framebuffer_set_font(int index)
{
    if (index < 0 || index >= framebuffer_font_count()) return 0;
    if (selected_font == index) return 1;
    selected_font = index;
    framebuffer_redraw_console();
    return 1;
}

void framebuffer_set_scale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    font_scale = scale;
    framebuffer_redraw_console();
}

static void framebuffer_put_codepoint(uint32_t codepoint, uint8_t color)
{
    framebuffer_console_t* console = current_console();
    const bitmap_font_t* font = fonts[selected_font];
    uint32_t columns;
    uint32_t rows;
    if (!info.available || !mapped || !font) return;
    int restore_pointer = pointer_drawn;
    int pointer_x = pointer_cell_x;
    int pointer_y = pointer_cell_y;
    framebuffer_pointer_restore();
    columns = framebuffer_columns(font);
    rows = framebuffer_rows(font);
    if (!columns || !rows) {
        if (restore_pointer) framebuffer_draw_pointer(pointer_x, pointer_y);
        return;
    }

    if (console->scroll_offset) {
        console->scroll_offset = 0;
        framebuffer_redraw_console();
    }

    if (codepoint == '\n') {
        framebuffer_advance_line(console, rows);
    } else if (codepoint == '\r') {
        console->cursor_column = 0;
    } else if (codepoint == '\b') {
        if (console->cursor_column) {
            console->cursor_column--;
            framebuffer_cell_t* cell =
                &console_line(console, console->cursor_line)
                     [console->cursor_column];
            cell->character = ' ';
            cell->color = color;
            if (console->cursor_line >=
                (console->cursor_line >= rows - 1 ?
                 console->cursor_line - (rows - 1) : 0))
                framebuffer_draw_glyph(
                    console->cursor_column,
                    (uint32_t)(console->cursor_line -
                        (console->cursor_line >= rows - 1 ?
                         console->cursor_line - (rows - 1) : 0)),
                    ' ', color);
        }
    } else {
        framebuffer_cell_t* cell =
            &console_line(console, console->cursor_line)[console->cursor_column];
        cell->character = codepoint;
        cell->color = color;
        uint64_t visible_start = console->cursor_line >= rows - 1 ?
                                 console->cursor_line - (rows - 1) : 0;
        if (console->cursor_line >= visible_start &&
            console->cursor_line - visible_start < rows)
            framebuffer_draw_glyph(console->cursor_column,
                (uint32_t)(console->cursor_line - visible_start),
                codepoint, color);
        console->cursor_column++;
    }
    if (console->cursor_column >= columns) {
        framebuffer_advance_line(console, rows);
    }
    if (restore_pointer) framebuffer_draw_pointer(pointer_x, pointer_y);
}

void framebuffer_putc(char c, uint8_t color)
{
    uint8_t byte = (uint8_t)c;
    uint32_t console_index = (uint32_t)active_console;
    if (console_index >= FRAMEBUFFER_CONSOLES)
        console_index = 0;

    if (!utf8_remaining[console_index]) {
        if (byte < 0x80) {
            framebuffer_put_codepoint(byte, color);
        } else if (byte >= 0xC2 && byte <= 0xDF) {
            utf8_remaining[console_index] = 1;
            utf8_codepoint[console_index] = byte & 0x1F;
            utf8_minimum[console_index] = 0x80;
        } else if (byte >= 0xE0 && byte <= 0xEF) {
            utf8_remaining[console_index] = 2;
            utf8_codepoint[console_index] = byte & 0x0F;
            utf8_minimum[console_index] = 0x800;
        } else if (byte >= 0xF0 && byte <= 0xF4) {
            utf8_remaining[console_index] = 3;
            utf8_codepoint[console_index] = byte & 0x07;
            utf8_minimum[console_index] = 0x10000;
        } else {
            framebuffer_put_codepoint('?', color);
        }
        return;
    }

    if ((byte & 0xC0) != 0x80) {
        utf8_remaining[console_index] = 0;
        framebuffer_put_codepoint('?', color);
        framebuffer_putc(c, color);
        return;
    }
    utf8_codepoint[console_index] =
        (utf8_codepoint[console_index] << 6) | (byte & 0x3F);
    if (--utf8_remaining[console_index])
        return;
    uint32_t codepoint = utf8_codepoint[console_index];
    if (codepoint < utf8_minimum[console_index] || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        codepoint = '?';
    framebuffer_put_codepoint(codepoint, color);
}

void framebuffer_write(const char* text, uint8_t color)
{
    if (!text) return;
    while (*text) framebuffer_putc(*text++, color);
}

void framebuffer_draw_pointer(int cell_x, int cell_y)
{
    const bitmap_font_t* font = fonts[selected_font];
    uint32_t draw_width =
        (uint32_t)(pointer_busy ? 16 : pointer_width) * POINTER_SCALE;
    uint32_t draw_height =
        (uint32_t)(pointer_busy ? 16 : pointer_sprite_height) * POINTER_SCALE;
    uint8_t hotspot_x = pointer_busy ? 7 : pointer_hotspot_x;
    uint8_t hotspot_y = pointer_busy ? 7 : pointer_hotspot_y;
    int x, y;
    if (!framebuffer_ready() || !font || info.bpp != 32) return;
    framebuffer_pointer_restore();
    if (cell_x < 0 || cell_y < 0 ||
        (uint32_t)cell_x >= info.width / (font->width * font_scale) ||
        (uint32_t)cell_y >= info.height / (font->height * font_scale))
        return;
    x = cell_x * font->width * font_scale -
        hotspot_x * POINTER_SCALE;
    y = cell_y * font->height * font_scale -
        hotspot_y * POINTER_SCALE;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (info.width >= draw_width &&
        (uint32_t)x + draw_width > info.width)
        x = (int)(info.width - draw_width);
    if (info.height >= draw_height &&
        (uint32_t)y + draw_height > info.height)
        y = (int)(info.height - draw_height);
    pointer_pixel_x = x;
    pointer_pixel_y = y;
    pointer_cell_x = cell_x;
    pointer_cell_y = cell_y;
    pointer_draw_width = (uint8_t)(draw_width / POINTER_SCALE);
    pointer_draw_height = (uint8_t)(draw_height / POINTER_SCALE);
    if (pointer_busy) {
        for (uint32_t row = 0; row < draw_height; row++) {
            for (uint32_t column = 0; column < draw_width; column++) {
                int screen_x = x + (int)column;
                int screen_y = y + (int)row;
                if (screen_x >= 0 && screen_y >= 0 &&
                    (uint32_t)screen_x < info.width &&
                    (uint32_t)screen_y < info.height)
                    pointer_saved[row * draw_width + column] =
                        framebuffer_read_pixel((uint32_t)screen_x,
                                               (uint32_t)screen_y);
            }
        }
        const char* const* frame =
            pointer_busy_frames[pointer_animation_phase & 3];
        for (uint32_t row = 0; row < draw_height; row++) {
            for (uint32_t column = 0; column < draw_width; column++) {
                uint32_t color;
                if (frame[row][column] == '1')
                    color = pointer_color;
                else if (frame[row][column] == '2')
                    color = pointer_outline;
                else
                    continue;
                uint32_t px = (uint32_t)x + column;
                uint32_t py = (uint32_t)y + row;
                if (px < info.width && py < info.height)
                    framebuffer_put_pixel(px, py, color);
            }
        }
        pointer_drawn = 1;
        return;
    }
    for (uint32_t row = 0; row < draw_height; row++) {
        for (uint32_t column = 0; column < draw_width; column++) {
            int screen_x = x + (int)column;
            int screen_y = y + (int)row;
            if (screen_x < 0 || screen_y < 0 ||
                (uint32_t)screen_x >= info.width ||
                (uint32_t)screen_y >= info.height)
                continue;
            pointer_saved[row * draw_width + column] =
                framebuffer_read_pixel((uint32_t)screen_x,
                                       (uint32_t)screen_y);
        }
    }
    if (pointer_has_explicit_outline) {
        for (uint32_t row = 0; row < draw_height; row++) {
            for (uint32_t column = 0; column < draw_width; column++) {
                uint32_t source_row = row / POINTER_SCALE;
                uint32_t source_column = column / POINTER_SCALE;
                if (!(pointer_outline_rows[source_row] &
                      (uint16_t)(1U <<
                          (pointer_width - source_column - 1))))
                    continue;
                int px = x + (int)column, py = y + (int)row;
                if (px >= 0 && py >= 0 && (uint32_t)px < info.width &&
                    (uint32_t)py < info.height)
                    framebuffer_put_pixel((uint32_t)px, (uint32_t)py,
                                          pointer_outline);
            }
        }
    } else if (pointer_outline) {
        for (uint32_t row = 0; row < draw_height; row++) {
            for (uint32_t column = 0; column < draw_width; column++) {
                uint32_t source_row = row / POINTER_SCALE;
                uint32_t source_column = column / POINTER_SCALE;
                if (pointer_rows[source_row] & (uint16_t)(1U <<
                        (pointer_width - source_column - 1)))
                    continue;
                int adjacent = 0;
                for (int dy = -1; dy <= 1 && !adjacent; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = (int)column + dx;
                        int ny = (int)row + dy;
                        if (nx < 0 || ny < 0 ||
                            nx >= (int)draw_width ||
                            ny >= (int)draw_height)
                            continue;
                        uint32_t neighbor_row =
                            (uint32_t)ny / POINTER_SCALE;
                        uint32_t neighbor_column =
                            (uint32_t)nx / POINTER_SCALE;
                        if (pointer_rows[neighbor_row] &
                            (uint16_t)(1U <<
                                (pointer_width - neighbor_column - 1))) {
                            adjacent = 1;
                            break;
                        }
                    }
                }
                if (adjacent) {
                    int px = x + (int)column, py = y + (int)row;
                    if (px >= 0 && py >= 0 && (uint32_t)px < info.width &&
                        (uint32_t)py < info.height)
                        framebuffer_put_pixel((uint32_t)px, (uint32_t)py,
                                              pointer_outline);
                }
            }
        }
    }
    for (uint32_t row = 0; row < draw_height; row++) {
        for (uint32_t column = 0; column < draw_width; column++) {
            uint32_t source_row = row / POINTER_SCALE;
            uint32_t source_column = column / POINTER_SCALE;
            if (!(pointer_rows[source_row] & (uint16_t)(1U <<
                    (pointer_width - source_column - 1))))
                continue;
            int px = x + (int)column, py = y + (int)row;
            if (px >= 0 && py >= 0 && (uint32_t)px < info.width &&
                (uint32_t)py < info.height)
                framebuffer_put_pixel((uint32_t)px, (uint32_t)py,
                                      pointer_color);
        }
    }
    pointer_drawn = 1;
}
