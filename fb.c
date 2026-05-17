/*
 * fb.c - Triumph OS framebuffer compositor
 *
 * Approach:
 *   - Always keep TTY in KD_TEXT mode (fbcon renders TTY text over framebuffer)
 *   - On boot: paint wallpaper to /dev/fb0, clear TTY, hide cursor
 *   - Shift+M: repaint wallpaper + panel, clear TTY, run real b_menu()
 *              after menu exits: repaint wallpaper, clear TTY
 *   - Shift+T: repaint wallpaper + panel, run real shell loop
 *              Shift+T again: repaint wallpaper, clear TTY
 *
 * The "transparency" effect: fbcon renders TTY text ON TOP of the framebuffer.
 * The framebuffer panel is blended with the wallpaper. TTY black backgrounds
 * are transparent to the framebuffer through fbcon. So you see:
 *   wallpaper → blended panel → TTY text on top = riced look
 *
 * External keyboards: /dev/input/event* thread
 */

#pragma once

#include <linux/fb.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <pthread.h>
#include "wallpaper.h"

/* ── panel style ─────────────────────────────────────────── */
#define PANEL_ALPHA  160
#define COL_PANEL_BG 0x050C18
#define COL_TITLEBAR 0x08122A
#define COL_BORDER   0x33CCFF

/* ── framebuffer ─────────────────────────────────────────── */
typedef struct {
    int fd, w, h, stride, bpp;
    int r_off, g_off, b_off;
    char *mem;
    size_t memsize;
    unsigned int *wp;
} FB;
static FB fb = {.fd=-1};

/* ── pixel ops ───────────────────────────────────────────── */
static inline unsigned int fb_blend(unsigned int bg, unsigned int fg, int a){
    unsigned int rb=bg&0xFF00FF, g_=bg&0x00FF00;
    unsigned int rb2=fg&0xFF00FF, g2=fg&0x00FF00;
    return (((rb*(256-a)+rb2*a)>>8)&0xFF00FF)|(((g_*(256-a)+g2*a)>>8)&0x00FF00);
}
static inline void fb_put(int x, int y, unsigned int rgb){
    if((unsigned)x>=(unsigned)fb.w||(unsigned)y>=(unsigned)fb.h) return;
    unsigned int r=(rgb>>16)&0xff,g=(rgb>>8)&0xff,b=rgb&0xff;
    if(fb.bpp==32)
        *(unsigned int*)(fb.mem+y*fb.stride+x*4)=(r<<fb.r_off)|(g<<fb.g_off)|(b<<fb.b_off);
    else if(fb.bpp==16)
        *(unsigned short*)(fb.mem+y*fb.stride+x*2)=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
}
static inline unsigned int wp_get(int x, int y){
    if(!fb.wp||(unsigned)x>=(unsigned)fb.w||(unsigned)y>=(unsigned)fb.h) return 0;
    return fb.wp[y*fb.w+x];
}

/* ── draw wallpaper ──────────────────────────────────────── */
static void fb_draw_wallpaper(void){
    if(fb.fd<0||!fb.wp) return;
    if(fb.bpp==32&&fb.r_off==16&&fb.g_off==8&&fb.b_off==0)
        for(int y=0;y<fb.h;y++)
            memcpy(fb.mem+y*fb.stride,fb.wp+y*fb.w,(size_t)fb.w*4);
    else
        for(int y=0;y<fb.h;y++)
            for(int x=0;x<fb.w;x++)
                fb_put(x,y,fb.wp[y*fb.w+x]);
}

/* ── draw transparent panel ──────────────────────────────── */
static void fb_draw_panel(int px, int py, int pw, int ph){
    if(fb.fd<0) return;
    for(int y=py;y<py+ph;y++)
        for(int x=px;x<px+pw;x++){
            unsigned int col=(y<py+4)?COL_TITLEBAR:COL_PANEL_BG;
            fb_put(x,y,fb_blend(wp_get(x,y),col,PANEL_ALPHA));
        }
    for(int x=px;x<px+pw;x++){
        fb_put(x,py,COL_BORDER);   fb_put(x,py+1,COL_BORDER);
        fb_put(x,py+ph-1,COL_BORDER); fb_put(x,py+ph-2,COL_BORDER);
    }
    for(int y=py;y<py+ph;y++){
        fb_put(px,y,COL_BORDER);   fb_put(px+1,y,COL_BORDER);
        fb_put(px+pw-1,y,COL_BORDER); fb_put(px+pw-2,y,COL_BORDER);
    }
    for(int dy=0;dy<8;dy++)
        for(int dx=0;dx<8-dy;dx++){
            fb_put(px+dx,     py+dy,     wp_get(px+dx,py+dy));
            fb_put(px+pw-1-dx,py+dy,     wp_get(px+pw-1-dx,py+dy));
            fb_put(px+dx,     py+ph-1-dy,wp_get(px+dx,py+ph-1-dy));
            fb_put(px+pw-1-dx,py+ph-1-dy,wp_get(px+pw-1-dx,py+ph-1-dy));
        }
}

/* ── clear TTY and hide cursor ───────────────────────────── */
static void tty_clear(void){
    write(1,"\x1b[2J\x1b[H\x1b[?25l",14);
    fflush(stdout);
}

/* ── geometry ────────────────────────────────────────────── */
static void menu_rect(int *px,int *py,int *pw,int *ph){
    *pw=fb.w*55/100; *ph=fb.h*75/100;
    *px=(fb.w-*pw)/2; *py=(fb.h-*ph)/2;
}
static void term_rect(int *px,int *py,int *pw,int *ph){
    int m=fb.w*2/100;
    *px=m;*py=m;*pw=fb.w-m*2;*ph=fb.h-m*2;
}

/* ── overlay state ───────────────────────────────────────── */
static int menu_open=0;
static int term_open=0;

/* ── toggle menu ─────────────────────────────────────────── */
static void fb_toggle_menu(void){
    if(fb.fd<0) return;
    if(menu_open){
        /* closing — repaint wallpaper, clear TTY */
        fb_draw_wallpaper();
        tty_clear();
        menu_open=0;
        return;
    }
    /* opening — paint wallpaper + panel, clear TTY so menu renders clean */
    menu_open=1;
    fb_draw_wallpaper();
    int px,py,pw,ph; menu_rect(&px,&py,&pw,&ph);
    fb_draw_panel(px,py,pw,ph);
    tty_clear();
    /* menu runs from triumph.c main loop */
}

static void fb_menu_post(void){
    /* after b_menu() returns */
    fb_draw_wallpaper();
    tty_clear();
    menu_open=0;
}

/* ── toggle terminal ─────────────────────────────────────── */
static void fb_toggle_term(void){
    if(fb.fd<0) return;
    if(term_open){
        fb_draw_wallpaper();
        tty_clear();
        term_open=0;
        return;
    }
    term_open=1;
    fb_draw_wallpaper();
    int px,py,pw,ph; term_rect(&px,&py,&pw,&ph);
    fb_draw_panel(px,py,pw,ph);
    tty_clear();
    /* shell loop runs from triumph.c main loop */
}

static void fb_term_post(void){
    fb_draw_wallpaper();
    tty_clear();
    term_open=0;
}

/* ── external keyboard thread ────────────────────────────── */
#define MAX_KBD 16
static int kbd_fds[MAX_KBD],kbd_nfds=0;
static volatile int kbd_shift=0;

static void kbd_scan(void){
    for(int i=0;i<kbd_nfds;i++) close(kbd_fds[i]); kbd_nfds=0;
    char path[64];
    for(int i=0;i<32&&kbd_nfds<MAX_KBD;i++){
        snprintf(path,sizeof(path),"/dev/input/event%d",i);
        int fd=open(path,O_RDONLY|O_NONBLOCK); if(fd<0) continue;
        unsigned long ev=0; ioctl(fd,EVIOCGBIT(0,sizeof(ev)),&ev);
        if((ev>>EV_KEY)&1) kbd_fds[kbd_nfds++]=fd; else close(fd);
    }
}

static void *kbd_thread(void *arg){
    (void)arg; kbd_scan();
    struct input_event ev;
    while(1){
        fd_set fds; FD_ZERO(&fds); int mx=0;
        for(int i=0;i<kbd_nfds;i++){FD_SET(kbd_fds[i],&fds);if(kbd_fds[i]>mx)mx=kbd_fds[i];}
        struct timeval tv={5,0};
        if(select(mx+1,&fds,NULL,NULL,&tv)<=0){kbd_scan();continue;}
        for(int i=0;i<kbd_nfds;i++){
            if(!FD_ISSET(kbd_fds[i],&fds)) continue;
            while(read(kbd_fds[i],&ev,sizeof(ev))==sizeof(ev)){
                if(ev.type!=EV_KEY) continue;
                if(ev.code==KEY_LEFTSHIFT||ev.code==KEY_RIGHTSHIFT)
                    kbd_shift=(ev.value!=0);
                /* external keyboard Shift+M/T are handled here
                   but since we cant call into the main readline loop,
                   we just write the sentinel to stdin so readline picks it up */
                if(ev.value==1&&kbd_shift){
                    if(ev.code==KEY_M){ write(0,"\x01M",2); }
                    if(ev.code==KEY_T){ write(0,"\x01T",2); }
                }
            }
        }
    }
    return NULL;
}

/* ── fb init ─────────────────────────────────────────────── */
static int fb_init(void){
    const char *devs[]={"/dev/fb0","/dev/fb1","/dev/graphics/fb0",NULL};
    for(int i=0;devs[i];i++){fb.fd=open(devs[i],O_RDWR);if(fb.fd>=0)break;}
    if(fb.fd<0) return -1;

    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if(ioctl(fb.fd,FBIOGET_VSCREENINFO,&vi)<0||
       ioctl(fb.fd,FBIOGET_FSCREENINFO,&fi)<0)
        {close(fb.fd);fb.fd=-1;return -1;}

    if(vi.bits_per_pixel!=32){
        vi.bits_per_pixel=32;
        ioctl(fb.fd,FBIOPUT_VSCREENINFO,&vi);
        ioctl(fb.fd,FBIOGET_VSCREENINFO,&vi);
        ioctl(fb.fd,FBIOGET_FSCREENINFO,&fi);
    }

    fb.w=vi.xres;fb.h=vi.yres;fb.bpp=vi.bits_per_pixel;
    fb.stride=fi.line_length;fb.memsize=(size_t)fi.line_length*vi.yres;
    fb.r_off=vi.red.offset;fb.g_off=vi.green.offset;fb.b_off=vi.blue.offset;

    fb.mem=mmap(NULL,fb.memsize,PROT_READ|PROT_WRITE,MAP_SHARED,fb.fd,0);
    if(fb.mem==MAP_FAILED){close(fb.fd);fb.fd=-1;return -1;}

    fb.wp=malloc(sizeof(unsigned int)*(size_t)fb.w*fb.h);
    if(fb.wp)
        for(int y=0;y<fb.h;y++){
            int sy=y*WP_H/fb.h;if(sy>=WP_H)sy=WP_H-1;
            for(int x=0;x<fb.w;x++){
                int sx=x*WP_W/fb.w;if(sx>=WP_W)sx=WP_W-1;
                fb.wp[y*fb.w+x]=wp_data[sy*WP_W+sx];
            }
        }
    return 0;
}

/* ── startup / shutdown ──────────────────────────────────── */
static void fb_startup(void){
    if(fb_init()<0) return;
    fb_draw_wallpaper();
    tty_clear();
    pthread_t t; pthread_create(&t,NULL,kbd_thread,NULL); pthread_detach(t);
}

static void fb_shutdown(void){
    if(fb.fd<0) return;
    write(1,"\x1b[?25h",6);
    free(fb.wp);fb.wp=NULL;
    munmap(fb.mem,fb.memsize);
    close(fb.fd);fb.fd=-1;
}
