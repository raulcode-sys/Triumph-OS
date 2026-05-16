/*
 * fb.c — Framebuffer compositor for Triumph OS
 *
 * Boot → pure wallpaper, no TTY clutter
 * Shift+M → transparent centered menu panel
 * Shift+T → transparent fullscreen terminal panel
 *
 * Supports: Intel, AMD, Nvidia (via efifb/GOP), VESA, simplefb
 * External keyboards: handled via /dev/input/event* in a background thread
 */

#pragma once

#include <linux/fb.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <pthread.h>
#include "wallpaper.h"

/* ── panel alpha: 180 = ~70% opaque, very glassy ── */
#define PANEL_ALPHA      170
#define TERM_ALPHA       150
#define PANEL_BG         0x00080F1E
#define PANEL_BORDER     0x0033CCFF
#define PANEL_TITLEBAR   0x000A1830
#define TERM_BG          0x00050D18
#define TERM_BORDER      0x0022AAEE

/* ── framebuffer ── */
typedef struct {
    int   fd;
    int   w, h, stride, bpp;
    int   r_off, g_off, b_off;
    char *mem;
    size_t memsize;
    unsigned int *wp_scaled;
} FB;

static FB g_fb = { .fd = -1 };

/* ── overlay state ── */
static int fb_menu_visible = 0;
static int fb_term_visible = 0;
static int fb_term_active  = 0; /* terminal has focus */

/* ── colour ops ── */
static inline unsigned int fb_blend(unsigned int bg, unsigned int fg, int a) {
    unsigned int rb  = bg & 0xFF00FF, g  = bg & 0x00FF00;
    unsigned int rb2 = fg & 0xFF00FF, g2 = fg & 0x00FF00;
    return (((rb*(256-a)+rb2*a)>>8)&0xFF00FF) | (((g*(256-a)+g2*a)>>8)&0x00FF00);
}

static inline void fb_put(int x, int y, unsigned int rgb) {
    FB *fb = &g_fb;
    if ((unsigned)x >= (unsigned)fb->w || (unsigned)y >= (unsigned)fb->h) return;
    unsigned int r=(rgb>>16)&0xff, g=(rgb>>8)&0xff, b=rgb&0xff;
    if (fb->bpp == 32) {
        *(unsigned int*)(fb->mem + y*fb->stride + x*4) =
            (r<<fb->r_off)|(g<<fb->g_off)|(b<<fb->b_off);
    } else if (fb->bpp == 16) {
        *(unsigned short*)(fb->mem + y*fb->stride + x*2) =
            ((r>>3)<<11)|((g>>2)<<5)|(b>>3);
    }
}

static inline unsigned int wp_get(int x, int y) {
    if (!g_fb.wp_scaled) return 0;
    if ((unsigned)x>=(unsigned)g_fb.w||(unsigned)y>=(unsigned)g_fb.h) return 0;
    return g_fb.wp_scaled[y*g_fb.w+x];
}

/* ── init ── */
static int fb_init(void) {
    const char *devs[] = {"/dev/fb0","/dev/fb1","/dev/graphics/fb0",NULL};
    for (int i=0; devs[i]; i++) { g_fb.fd=open(devs[i],O_RDWR); if(g_fb.fd>=0) break; }
    if (g_fb.fd < 0) return -1;

    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if (ioctl(g_fb.fd,FBIOGET_VSCREENINFO,&vi)<0||ioctl(g_fb.fd,FBIOGET_FSCREENINFO,&fi)<0)
        { close(g_fb.fd); g_fb.fd=-1; return -1; }

    /* try force 32bpp */
    if (vi.bits_per_pixel != 32) {
        vi.bits_per_pixel = 32;
        ioctl(g_fb.fd,FBIOPUT_VSCREENINFO,&vi);
        ioctl(g_fb.fd,FBIOGET_VSCREENINFO,&vi);
        ioctl(g_fb.fd,FBIOGET_FSCREENINFO,&fi);
    }

    g_fb.w=vi.xres; g_fb.h=vi.yres; g_fb.bpp=vi.bits_per_pixel;
    g_fb.stride=fi.line_length; g_fb.memsize=(size_t)fi.line_length*vi.yres;
    g_fb.r_off=vi.red.offset; g_fb.g_off=vi.green.offset; g_fb.b_off=vi.blue.offset;

    g_fb.mem=mmap(NULL,g_fb.memsize,PROT_READ|PROT_WRITE,MAP_SHARED,g_fb.fd,0);
    if (g_fb.mem==MAP_FAILED) { close(g_fb.fd); g_fb.fd=-1; return -1; }

    /* pre-scale wallpaper */
    g_fb.wp_scaled = malloc(sizeof(unsigned int)*(size_t)g_fb.w*g_fb.h);
    if (g_fb.wp_scaled) {
        for (int y=0; y<g_fb.h; y++) {
            int sy=y*WP_H/g_fb.h; if(sy>=WP_H)sy=WP_H-1;
            for (int x=0; x<g_fb.w; x++) {
                int sx=x*WP_W/g_fb.w; if(sx>=WP_W)sx=WP_W-1;
                g_fb.wp_scaled[y*g_fb.w+x]=wp_data[sy*WP_W+sx];
            }
        }
    }
    return 0;
}

static void fb_close(void) {
    if (g_fb.fd<0) return;
    free(g_fb.wp_scaled); g_fb.wp_scaled=NULL;
    munmap(g_fb.mem,g_fb.memsize);
    close(g_fb.fd); g_fb.fd=-1;
}

/* ── wallpaper ── */
static void fb_draw_wallpaper(void) {
    if (g_fb.fd<0||!g_fb.wp_scaled) return;
    if (g_fb.bpp==32&&g_fb.r_off==16&&g_fb.g_off==8&&g_fb.b_off==0) {
        for (int y=0; y<g_fb.h; y++)
            memcpy(g_fb.mem+y*g_fb.stride, g_fb.wp_scaled+y*g_fb.w, (size_t)g_fb.w*4);
    } else {
        for (int y=0; y<g_fb.h; y++)
            for (int x=0; x<g_fb.w; x++)
                fb_put(x,y,g_fb.wp_scaled[y*g_fb.w+x]);
    }
}

static void fb_restore(int px, int py, int pw, int ph) {
    for (int y=py; y<py+ph&&y<g_fb.h; y++)
        for (int x=px; x<px+pw&&x<g_fb.w; x++)
            fb_put(x,y,wp_get(x,y));
}

/* ── panel drawing ── */
static void fb_panel(int px, int py, int pw, int ph,
                     unsigned int bg, unsigned int title_bg,
                     unsigned int border, int alpha) {
    /* fill */
    for (int y=py; y<py+ph; y++)
        for (int x=px; x<px+pw; x++) {
            unsigned int col = (y<py+32) ? title_bg : bg;
            fb_put(x,y,fb_blend(wp_get(x,y),col,alpha));
        }
    /* border 2px */
    for (int x=px; x<px+pw; x++) {
        fb_put(x,py,border);   fb_put(x,py+1,border);
        fb_put(x,py+ph-1,border); fb_put(x,py+ph-2,border);
    }
    for (int y=py; y<py+ph; y++) {
        fb_put(px,y,border);   fb_put(px+1,y,border);
        fb_put(px+pw-1,y,border); fb_put(px+pw-2,y,border);
    }
    /* rounded corners r=10 */
    for (int dy=0; dy<10; dy++)
        for (int dx=0; dx<10-dy; dx++) {
            fb_put(px+dx,      py+dy,      wp_get(px+dx,py+dy));
            fb_put(px+pw-1-dx, py+dy,      wp_get(px+pw-1-dx,py+dy));
            fb_put(px+dx,      py+ph-1-dy, wp_get(px+dx,py+ph-1-dy));
            fb_put(px+pw-1-dx, py+ph-1-dy, wp_get(px+pw-1-dx,py+ph-1-dy));
        }
}

/* ── geometry ── */
static void fb_menu_rect(int *px,int *py,int *pw,int *ph) {
    *pw=g_fb.w*52/100; *ph=g_fb.h*68/100;
    *px=(g_fb.w-*pw)/2; *py=(g_fb.h-*ph)/2;
}

static void fb_term_rect(int *px,int *py,int *pw,int *ph) {
    /* fullscreen with small margin */
    int margin = g_fb.w*2/100;
    *px=margin; *py=margin;
    *pw=g_fb.w-margin*2; *ph=g_fb.h-margin*2;
}

/* ── toggle menu ── */
static void fb_toggle_menu(void) {
    if (g_fb.fd<0) return;
    int px,py,pw,ph; fb_menu_rect(&px,&py,&pw,&ph);
    if (fb_menu_visible) {
        fb_restore(px,py,pw,ph);
        fb_menu_visible=0;
        /* redraw term if visible */
        if (fb_term_visible) {
            int tx,ty,tw,th; fb_term_rect(&tx,&ty,&tw,&th);
            fb_panel(tx,ty,tw,th,TERM_BG,PANEL_TITLEBAR,TERM_BORDER,TERM_ALPHA);
        }
    } else {
        fb_panel(px,py,pw,ph,PANEL_BG,PANEL_TITLEBAR,PANEL_BORDER,PANEL_ALPHA);
        fb_menu_visible=1;
    }
}

/* ── toggle terminal ── */
static void fb_toggle_term(void) {
    if (g_fb.fd<0) return;
    int px,py,pw,ph; fb_term_rect(&px,&py,&pw,&ph);
    if (fb_term_visible) {
        fb_restore(px,py,pw,ph);
        fb_term_visible=0; fb_term_active=0;
        /* redraw menu if visible */
        if (fb_menu_visible) {
            int mx,my,mw,mh; fb_menu_rect(&mx,&my,&mw,&mh);
            fb_panel(mx,my,mw,mh,PANEL_BG,PANEL_TITLEBAR,PANEL_BORDER,PANEL_ALPHA);
        }
    } else {
        fb_panel(px,py,pw,ph,TERM_BG,PANEL_TITLEBAR,TERM_BORDER,TERM_ALPHA);
        fb_term_visible=1; fb_term_active=1;
    }
}

/* ── external keyboard input thread ── */
/*
 * Scans /dev/input/event* for keyboards and watches for:
 *   KEY_LEFTSHIFT/KEY_RIGHTSHIFT + KEY_M → toggle menu
 *   KEY_LEFTSHIFT/KEY_RIGHTSHIFT + KEY_T → toggle terminal
 * This works for USB/Bluetooth external keyboards.
 */

#define MAX_KBD 16
static int kbd_fds[MAX_KBD];
static int kbd_nfds = 0;
static int kbd_shift = 0;

static int is_keyboard(const char *path) {
    int fd = open(path, O_RDONLY|O_NONBLOCK);
    if (fd < 0) return 0;
    unsigned long evbit = 0;
    ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);
    close(fd);
    return (evbit >> EV_KEY) & 1;
}

static void kbd_scan(void) {
    kbd_nfds = 0;
    char path[64];
    for (int i = 0; i < 32 && kbd_nfds < MAX_KBD; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (is_keyboard(path)) {
            int fd = open(path, O_RDONLY|O_NONBLOCK);
            if (fd >= 0) kbd_fds[kbd_nfds++] = fd;
        }
    }
}

static void *kbd_thread(void *arg) {
    (void)arg;
    kbd_scan();
    struct input_event ev;
    while (1) {
        /* rescan every 5s for hotplug */
        fd_set fds; FD_ZERO(&fds);
        int maxfd = 0;
        for (int i=0; i<kbd_nfds; i++) {
            FD_SET(kbd_fds[i], &fds);
            if (kbd_fds[i] > maxfd) maxfd = kbd_fds[i];
        }
        struct timeval tv = {5, 0};
        int r = select(maxfd+1, &fds, NULL, NULL, &tv);
        if (r <= 0) { kbd_scan(); continue; }

        for (int i=0; i<kbd_nfds; i++) {
            if (!FD_ISSET(kbd_fds[i], &fds)) continue;
            while (read(kbd_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type != EV_KEY) continue;
                if (ev.code==KEY_LEFTSHIFT||ev.code==KEY_RIGHTSHIFT)
                    kbd_shift = (ev.value != 0);
                if (ev.value==1 && kbd_shift) {
                    if (ev.code==KEY_M) fb_toggle_menu();
                    if (ev.code==KEY_T) fb_toggle_term();
                }
            }
        }
    }
    return NULL;
}

/* ── startup / shutdown ── */
static void fb_startup(void) {
    if (fb_init() < 0) return;
    /* hide cursor, clear TTY */
    write(1, "\x1b[?25l\x1b[2J\x1b[H", 15);
    fb_draw_wallpaper();
    /* start external keyboard listener */
    pthread_t t;
    pthread_create(&t, NULL, kbd_thread, NULL);
    pthread_detach(t);
}

static void fb_shutdown(void) {
    if (g_fb.fd<0) return;
    int px,py,pw,ph;
    if (fb_menu_visible){fb_menu_rect(&px,&py,&pw,&ph);fb_restore(px,py,pw,ph);}
    if (fb_term_visible){fb_term_rect(&px,&py,&pw,&ph);fb_restore(px,py,pw,ph);}
    fb_close();
    write(1,"\x1b[?25h",6);
}
