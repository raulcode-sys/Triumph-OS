/*
 * fb.c — Framebuffer wallpaper + semi-transparent panel compositor
 * Triumph OS
 *
 * Supports:
 *   - Intel/AMD via vesafb / efifb
 *   - Nvidia via efifb (UEFI GOP) — works without proprietary drivers
 *   - simplefb (QEMU, VMs, some ARM)
 *   - Graceful fallback: if no fb available, everything still works in TTY
 *
 * Key bindings (at shell prompt):
 *   Shift+M  → toggle menu panel
 *   Shift+T  → toggle terminal panel
 */

#pragma once

#include <linux/fb.h>
#include <sys/mman.h>
#include "wallpaper.h"

typedef struct {
    int   fd;
    int   w, h;
    int   stride;
    int   bpp;
    int   r_off, g_off, b_off;
    char *mem;
    size_t memsize;
    unsigned int *wp_scaled;
} FB;

static FB g_fb = { .fd = -1 };

static unsigned int fb_blend(unsigned int bg, unsigned int fg, int alpha) {
    unsigned int rb  = bg & 0xFF00FF, g  = bg & 0x00FF00;
    unsigned int rb2 = fg & 0xFF00FF, g2 = fg & 0x00FF00;
    rb = ((rb  * (256 - alpha) + rb2 * alpha) >> 8) & 0xFF00FF;
    g  = ((g   * (256 - alpha) + g2  * alpha) >> 8) & 0x00FF00;
    return rb | g;
}

static void fb_put(FB *fb, int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return;
    unsigned int r = (rgb >> 16) & 0xff;
    unsigned int g = (rgb >>  8) & 0xff;
    unsigned int b = (rgb      ) & 0xff;
    if (fb->bpp == 32) {
        unsigned int pix = (r << fb->r_off) | (g << fb->g_off) | (b << fb->b_off);
        *(unsigned int *)(fb->mem + y * fb->stride + x * 4) = pix;
    } else if (fb->bpp == 16) {
        unsigned short pix = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        *(unsigned short *)(fb->mem + y * fb->stride + x * 2) = pix;
    } else if (fb->bpp == 24) {
        unsigned char *p = (unsigned char *)(fb->mem + y * fb->stride + x * 3);
        p[fb->r_off/8] = r; p[fb->g_off/8] = g; p[fb->b_off/8] = b;
    }
}

static unsigned int wp_pixel(FB *fb, int x, int y) {
    if (!fb->wp_scaled) return 0;
    if (x < 0 || x >= fb->w || y < 0 || y >= fb->h) return 0;
    return fb->wp_scaled[y * fb->w + x];
}

static int fb_init(void) {
    const char *devs[] = { "/dev/fb0", "/dev/fb1", "/dev/graphics/fb0", NULL };
    for (int i = 0; devs[i]; i++) {
        g_fb.fd = open(devs[i], O_RDWR);
        if (g_fb.fd >= 0) break;
    }
    if (g_fb.fd < 0) return -1;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(g_fb.fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(g_fb.fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(g_fb.fd); g_fb.fd = -1; return -1;
    }

    if (vinfo.bits_per_pixel != 32) {
        vinfo.bits_per_pixel = 32;
        ioctl(g_fb.fd, FBIOPUT_VSCREENINFO, &vinfo);
        ioctl(g_fb.fd, FBIOGET_VSCREENINFO, &vinfo);
        ioctl(g_fb.fd, FBIOGET_FSCREENINFO, &finfo);
    }

    g_fb.w      = vinfo.xres;
    g_fb.h      = vinfo.yres;
    g_fb.bpp    = vinfo.bits_per_pixel;
    g_fb.stride = finfo.line_length;
    g_fb.memsize = (size_t)finfo.line_length * vinfo.yres;
    g_fb.r_off  = vinfo.red.offset;
    g_fb.g_off  = vinfo.green.offset;
    g_fb.b_off  = vinfo.blue.offset;

    g_fb.mem = mmap(NULL, g_fb.memsize, PROT_READ|PROT_WRITE,
                    MAP_SHARED, g_fb.fd, 0);
    if (g_fb.mem == MAP_FAILED) {
        close(g_fb.fd); g_fb.fd = -1; return -1;
    }

    g_fb.wp_scaled = malloc(sizeof(unsigned int) * (size_t)g_fb.w * g_fb.h);
    if (g_fb.wp_scaled) {
        for (int y = 0; y < g_fb.h; y++) {
            int sy = y * WP_H / g_fb.h; if (sy >= WP_H) sy = WP_H-1;
            for (int x = 0; x < g_fb.w; x++) {
                int sx = x * WP_W / g_fb.w; if (sx >= WP_W) sx = WP_W-1;
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
    close(g_fb.fd); g_fb.fd = -1;
}

static void fb_draw_wallpaper(void) {
    if (g_fb.fd < 0 || !g_fb.wp_scaled) return;
    if (g_fb.bpp == 32 && g_fb.r_off == 16 && g_fb.g_off == 8 && g_fb.b_off == 0) {
        for (int y = 0; y < g_fb.h; y++) {
            unsigned int *dst = (unsigned int *)(g_fb.mem + y * g_fb.stride);
            memcpy(dst, g_fb.wp_scaled + y * g_fb.w, (size_t)g_fb.w * 4);
        }
    } else {
        for (int y = 0; y < g_fb.h; y++)
            for (int x = 0; x < g_fb.w; x++)
                fb_put(&g_fb, x, y, g_fb.wp_scaled[y * g_fb.w + x]);
    }
}

#define PANEL_ALPHA    160
#define PANEL_BG       0x00050D1A
#define PANEL_TITLEBAR 0x000A1A30
#define PANEL_BORDER   0x0033BBFF

static void fb_draw_panel(int px, int py, int pw, int ph) {
    if (g_fb.fd < 0) return;
    for (int y = py; y < py+ph; y++)
        for (int x = px; x < px+pw; x++) {
            unsigned int col = (y < py+28) ? PANEL_TITLEBAR : PANEL_BG;
            fb_put(&g_fb, x, y, fb_blend(wp_pixel(&g_fb,x,y), col, PANEL_ALPHA));
        }
    for (int x = px; x < px+pw; x++) {
        fb_put(&g_fb,x,py,      PANEL_BORDER); fb_put(&g_fb,x,py+1,    PANEL_BORDER);
        fb_put(&g_fb,x,py+ph-1,PANEL_BORDER); fb_put(&g_fb,x,py+ph-2, PANEL_BORDER);
    }
    for (int y = py; y < py+ph; y++) {
        fb_put(&g_fb,px,     y,PANEL_BORDER); fb_put(&g_fb,px+1,    y,PANEL_BORDER);
        fb_put(&g_fb,px+pw-1,y,PANEL_BORDER); fb_put(&g_fb,px+pw-2,y,PANEL_BORDER);
    }
    int r = 8;
    for (int dy = 0; dy < r; dy++)
        for (int dx = 0; dx < r-dy; dx++) {
            fb_put(&g_fb,px+dx,      py+dy,      wp_pixel(&g_fb,px+dx,py+dy));
            fb_put(&g_fb,px+pw-1-dx, py+dy,      wp_pixel(&g_fb,px+pw-1-dx,py+dy));
            fb_put(&g_fb,px+dx,      py+ph-1-dy, wp_pixel(&g_fb,px+dx,py+ph-1-dy));
            fb_put(&g_fb,px+pw-1-dx, py+ph-1-dy, wp_pixel(&g_fb,px+pw-1-dx,py+ph-1-dy));
        }
}

static void fb_restore_region(int px, int py, int pw, int ph) {
    if (g_fb.fd < 0) return;
    for (int y = py; y < py+ph && y < g_fb.h; y++)
        for (int x = px; x < px+pw && x < g_fb.w; x++)
            fb_put(&g_fb, x, y, wp_pixel(&g_fb, x, y));
}

static void fb_menu_rect(int *px, int *py, int *pw, int *ph) {
    *pw = g_fb.w*55/100; *ph = g_fb.h*70/100;
    *px = (g_fb.w - *pw)/2; *py = (g_fb.h - *ph)/2;
}

static void fb_term_rect(int *px, int *py, int *pw, int *ph) {
    *pw = g_fb.w*45/100; *ph = g_fb.h*55/100;
    *px = g_fb.w - *pw - g_fb.w*3/100; *py = g_fb.h*5/100;
}

static int fb_menu_visible = 0;
static int fb_term_visible = 0;

static void fb_toggle_menu(void) {
    if (g_fb.fd < 0) return;
    int px,py,pw,ph; fb_menu_rect(&px,&py,&pw,&ph);
    if (fb_menu_visible) { fb_restore_region(px,py,pw,ph); fb_menu_visible=0; }
    else                 { fb_draw_panel(px,py,pw,ph);     fb_menu_visible=1; }
}

static void fb_toggle_term(void) {
    if (g_fb.fd < 0) return;
    int px,py,pw,ph; fb_term_rect(&px,&py,&pw,&ph);
    if (fb_term_visible) { fb_restore_region(px,py,pw,ph); fb_term_visible=0; }
    else                 { fb_draw_panel(px,py,pw,ph);     fb_term_visible=1; }
}

static void fb_startup(void) {
    if (fb_init() < 0) return;
    fb_draw_wallpaper();
}

static void fb_shutdown(void) {
    if (g_fb.fd < 0) return;
    int px,py,pw,ph;
    if (fb_menu_visible) { fb_menu_rect(&px,&py,&pw,&ph); fb_restore_region(px,py,pw,ph); }
    if (fb_term_visible) { fb_term_rect(&px,&py,&pw,&ph); fb_restore_region(px,py,pw,ph); }
    fb_close();
}
