/*
 * fb.c — Framebuffer wallpaper + semi-transparent panel compositor
 * For Triumph OS
 *
 * Renders the Berserk wallpaper to /dev/fb0, then draws semi-transparent
 * panels (menu, terminal) on top by blending pixels with the wallpaper.
 *
 * Key bindings (captured in triumph_readline):
 *   Shift+M  → toggle menu overlay
 *   Shift+T  → toggle terminal overlay
 */

#pragma once

#include <linux/fb.h>
#include "wallpaper.h"

/* ── framebuffer state ─────────────────────────────────────────────── */

typedef struct {
    int   fd;
    int   w, h;
    int   stride;       /* bytes per line */
    int   bpp;          /* bits per pixel */
    char *mem;          /* mmap'd fb */
    size_t memsize;

    /* saved wallpaper region for panel restore */
    unsigned int *wp_scaled; /* WP_W×WP_H scaled to fb res, XRGB */
} FB;

static FB g_fb = { .fd = -1 };

/* ── colour helpers ────────────────────────────────────────────────── */

static unsigned int fb_blend(unsigned int bg, unsigned int fg, int alpha) {
    /* alpha 0..255: 0=fully transparent fg, 255=fully opaque fg */
    unsigned int rb = bg & 0xFF00FF;
    unsigned int g  = bg & 0x00FF00;
    unsigned int rb2 = fg & 0xFF00FF;
    unsigned int g2  = fg & 0x00FF00;
    rb = ((rb * (256 - alpha) + rb2 * alpha) >> 8) & 0xFF00FF;
    g  = ((g  * (256 - alpha) + g2  * alpha) >> 8) & 0x00FF00;
    return rb | g;
}

/* Write a pixel to the framebuffer at (x,y) */
static void fb_put(FB *fb, int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return;
    if (fb->bpp == 32) {
        unsigned int *p = (unsigned int *)(fb->mem + y * fb->stride + x * 4);
        *p = rgb;
    } else if (fb->bpp == 16) {
        unsigned short r = (rgb >> 19) & 0x1f;
        unsigned short g2 = (rgb >> 10) & 0x3f;
        unsigned short b = (rgb >>  3) & 0x1f;
        unsigned short *p = (unsigned short *)(fb->mem + y * fb->stride + x * 2);
        *p = (r << 11) | (g2 << 5) | b;
    }
}

/* Read wallpaper pixel, scaled to fb resolution */
static unsigned int wp_pixel(FB *fb, int x, int y) {
    if (!fb->wp_scaled) return 0;
    if (x < 0 || x >= fb->w || y < 0 || y >= fb->h) return 0;
    /* nearest-neighbour sample from WP_W×WP_H → fb->w×fb->h */
    int sx = x * WP_W / fb->w;
    int sy = y * WP_H / fb->h;
    if (sx >= WP_W) sx = WP_W - 1;
    if (sy >= WP_H) sy = WP_H - 1;
    return wp_data[sy * WP_W + sx];
}

/* ── init / teardown ───────────────────────────────────────────────── */

static int fb_init(void) {
    g_fb.fd = open("/dev/fb0", O_RDWR);
    if (g_fb.fd < 0) return -1;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(g_fb.fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(g_fb.fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(g_fb.fd); g_fb.fd = -1; return -1;
    }

    g_fb.w      = vinfo.xres;
    g_fb.h      = vinfo.yres;
    g_fb.bpp    = vinfo.bits_per_pixel;
    g_fb.stride = finfo.line_length;
    g_fb.memsize = (size_t)finfo.line_length * vinfo.yres;

    g_fb.mem = mmap(NULL, g_fb.memsize, PROT_READ|PROT_WRITE,
                    MAP_SHARED, g_fb.fd, 0);
    if (g_fb.mem == MAP_FAILED) {
        close(g_fb.fd); g_fb.fd = -1; return -1;
    }

    /* pre-scale wallpaper to fb resolution (store as XRGB ints) */
    g_fb.wp_scaled = malloc(sizeof(unsigned int) * (size_t)g_fb.w * g_fb.h);
    if (g_fb.wp_scaled) {
        for (int y = 0; y < g_fb.h; y++) {
            int sy = y * WP_H / g_fb.h;
            if (sy >= WP_H) sy = WP_H - 1;
            for (int x = 0; x < g_fb.w; x++) {
                int sx = x * WP_W / g_fb.w;
                if (sx >= WP_W) sx = WP_W - 1;
                g_fb.wp_scaled[y * g_fb.w + x] = wp_data[sy * WP_W + sx];
            }
        }
    }

    return 0;
}

static void fb_close(void) {
    if (g_fb.fd < 0) return;
    free(g_fb.wp_scaled); g_fb.wp_scaled = NULL;
    munmap(g_fb.mem, g_fb.memsize);
    close(g_fb.fd);
    g_fb.fd = -1;
}

/* ── wallpaper rendering ───────────────────────────────────────────── */

static void fb_draw_wallpaper(void) {
    if (g_fb.fd < 0 || !g_fb.wp_scaled) return;
    if (g_fb.bpp == 32) {
        for (int y = 0; y < g_fb.h; y++) {
            unsigned int *dst = (unsigned int *)(g_fb.mem + y * g_fb.stride);
            const unsigned int *src = g_fb.wp_scaled + y * g_fb.w;
            memcpy(dst, src, (size_t)g_fb.w * 4);
        }
    } else {
        for (int y = 0; y < g_fb.h; y++)
            for (int x = 0; x < g_fb.w; x++)
                fb_put(&g_fb, x, y, g_fb.wp_scaled[y * g_fb.w + x]);
    }
}

/* ── semi-transparent panel ────────────────────────────────────────── */

/* Panel alpha: 0=invisible, 255=opaque. 160 ≈ 63% opacity = "kinda" transparent */
#define PANEL_ALPHA       160
/* Panel background colour (dark navy, matches Triumph theme) */
#define PANEL_BG          0x00050D1A
/* Panel border colour */
#define PANEL_BORDER      0x0033BBFF
/* Panel title bar colour */
#define PANEL_TITLEBAR    0x000A1A30

/*
 * Draw a semi-transparent rounded panel at (px,py) size (pw×ph).
 * Content is blended over the wallpaper.
 */
static void fb_draw_panel(int px, int py, int pw, int ph,
                           const char *title, int is_terminal) {
    if (g_fb.fd < 0) return;

    /* ── fill body ── */
    for (int y = py; y < py + ph; y++) {
        for (int x = px; x < px + pw; x++) {
            unsigned int wp = wp_pixel(&g_fb, x, y);
            unsigned int col = PANEL_BG;

            /* title bar slightly lighter */
            if (y < py + 28) col = PANEL_TITLEBAR;

            unsigned int blended = fb_blend(wp, col, PANEL_ALPHA);
            fb_put(&g_fb, x, y, blended);
        }
    }

    /* ── border ── */
    for (int x = px; x < px + pw; x++) {
        fb_put(&g_fb, x, py,        PANEL_BORDER);
        fb_put(&g_fb, x, py + ph-1, PANEL_BORDER);
        /* 2px border */
        fb_put(&g_fb, x, py+1,      PANEL_BORDER);
        fb_put(&g_fb, x, py + ph-2, PANEL_BORDER);
    }
    for (int y = py; y < py + ph; y++) {
        fb_put(&g_fb, px,      y, PANEL_BORDER);
        fb_put(&g_fb, px+1,    y, PANEL_BORDER);
        fb_put(&g_fb, px+pw-1, y, PANEL_BORDER);
        fb_put(&g_fb, px+pw-2, y, PANEL_BORDER);
    }

    /* ── corner radius (erase corners to expose wallpaper) ── */
    int r = 8;
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r - dy; dx++) {
            /* top-left */
            fb_put(&g_fb, px+dx,    py+dy,    wp_pixel(&g_fb,px+dx,py+dy));
            /* top-right */
            fb_put(&g_fb, px+pw-1-dx, py+dy,  wp_pixel(&g_fb,px+pw-1-dx,py+dy));
            /* bottom-left */
            fb_put(&g_fb, px+dx,    py+ph-1-dy, wp_pixel(&g_fb,px+dx,py+ph-1-dy));
            /* bottom-right */
            fb_put(&g_fb, px+pw-1-dx, py+ph-1-dy,
                   wp_pixel(&g_fb,px+pw-1-dx,py+ph-1-dy));
        }
    }

    (void)title; (void)is_terminal;
}

/*
 * Restore wallpaper behind a panel region (call before hiding panel)
 */
static void fb_restore_region(int px, int py, int pw, int ph) {
    if (g_fb.fd < 0) return;
    for (int y = py; y < py + ph && y < g_fb.h; y++)
        for (int x = px; x < px + pw && x < g_fb.w; x++)
            fb_put(&g_fb, x, y, wp_pixel(&g_fb, x, y));
}

/* ── panel geometry helpers ────────────────────────────────────────── */

/* Menu panel: centred, 55% wide, 70% tall */
static void fb_menu_rect(int *px, int *py, int *pw, int *ph) {
    *pw = g_fb.w * 55 / 100;
    *ph = g_fb.h * 70 / 100;
    *px = (g_fb.w - *pw) / 2;
    *py = (g_fb.h - *ph) / 2;
}

/* Terminal panel: right side, 45% wide, 55% tall */
static void fb_term_rect(int *px, int *py, int *pw, int *ph) {
    *pw = g_fb.w * 45 / 100;
    *ph = g_fb.h * 55 / 100;
    *px = g_fb.w - *pw - g_fb.w * 3 / 100;
    *py = g_fb.h * 5 / 100;
}

/* ── global overlay state ──────────────────────────────────────────── */

static int fb_menu_visible = 0;
static int fb_term_visible = 0;

static void fb_show_menu_panel(void) {
    int px, py, pw, ph;
    fb_menu_rect(&px, &py, &pw, &ph);
    fb_draw_panel(px, py, pw, ph, "Triumph Menu", 0);
    fb_menu_visible = 1;
}

static void fb_hide_menu_panel(void) {
    if (!fb_menu_visible) return;
    int px, py, pw, ph;
    fb_menu_rect(&px, &py, &pw, &ph);
    fb_restore_region(px, py, pw, ph);
    fb_menu_visible = 0;
}

static void fb_show_term_panel(void) {
    int px, py, pw, ph;
    fb_term_rect(&px, &py, &pw, &ph);
    fb_draw_panel(px, py, pw, ph, "Terminal", 1);
    fb_term_visible = 1;
}

static void fb_hide_term_panel(void) {
    if (!fb_term_visible) return;
    int px, py, pw, ph;
    fb_term_rect(&px, &py, &pw, &ph);
    fb_restore_region(px, py, pw, ph);
    fb_term_visible = 0;
}

/*
 * Toggle menu panel (called on Shift+M)
 */
static void fb_toggle_menu(void) {
    if (g_fb.fd < 0) return;
    if (fb_menu_visible) fb_hide_menu_panel();
    else                 fb_show_menu_panel();
}

/*
 * Toggle terminal panel (called on Shift+T)
 */
static void fb_toggle_term(void) {
    if (g_fb.fd < 0) return;
    if (fb_term_visible) fb_hide_term_panel();
    else                 fb_show_term_panel();
}

/*
 * Call once at startup: init fb, paint wallpaper.
 * Silent if /dev/fb0 unavailable (graceful degradation).
 */
static void fb_startup(void) {
    if (fb_init() < 0) return;      /* no framebuffer, that's fine */
    fb_draw_wallpaper();
}

/*
 * Call at shutdown.
 */
static void fb_shutdown(void) {
    fb_hide_menu_panel();
    fb_hide_term_panel();
    fb_close();
}
