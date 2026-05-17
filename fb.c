/*
 * fb.c - Triumph OS framebuffer compositor
 *
 * - Boots to pure wallpaper (TTY suppressed via KD_GRAPHICS)
 * - Shift+M → real interactive menu panel drawn over wallpaper
 * - Shift+T → real interactive terminal (pty) drawn over wallpaper
 * - Shift+T while terminal open → closes it
 * - External keyboards via /dev/input/event*
 * - All text rendered as pixels via built-in bitmap font
 * - No TTY text ever shown
 */

#pragma once

#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <pty.h>
#include <pthread.h>
#include "wallpaper.h"

/* ── colours & alpha ─────────────────────────────────────── */
#define PANEL_ALPHA   165
#define TERM_ALPHA    150
#define COL_PANEL_BG  0x060D1A
#define COL_TERM_BG   0x030810
#define COL_TITLEBAR  0x0A1428
#define COL_BORDER    0x33CCFF
#define COL_TERM_BOR  0x1188CC
#define COL_TEXT      0xCCEEFF
#define COL_DIM       0x446688
#define COL_ACCENT    0x33CCFF
#define COL_GREEN     0x44FF88
#define COL_SEL_BG    0x0D2A44
#define COL_SEL_FG    0x33CCFF

/* ── framebuffer ─────────────────────────────────────────── */
typedef struct {
    int fd, w, h, stride, bpp;
    int r_off, g_off, b_off;
    char *mem;
    size_t memsize;
    unsigned int *wp;
} FB;
static FB fb = {.fd=-1};

/* ── pixel blending ──────────────────────────────────────── */
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
static inline unsigned int wp_get(int x,int y){
    if(!fb.wp||(unsigned)x>=(unsigned)fb.w||(unsigned)y>=(unsigned)fb.h) return 0;
    return fb.wp[y*fb.w+x];
}

/* ── bitmap font 6×10 ────────────────────────────────────── */
static const unsigned char FONT[95][10]={
{0,0,0,0,0,0,0,0,0,0},
{0x08,0x08,0x08,0x08,0x08,0,0x08,0,0,0},
{0x14,0x14,0,0,0,0,0,0,0,0},
{0x14,0x14,0x3E,0x14,0x3E,0x14,0x14,0,0,0},
{0x08,0x1E,0x28,0x1C,0x0A,0x3C,0x08,0,0,0},
{0x30,0x32,0x04,0x08,0x10,0x26,0x06,0,0,0},
{0x10,0x28,0x28,0x10,0x2A,0x24,0x1A,0,0,0},
{0x08,0x08,0,0,0,0,0,0,0,0},
{0x04,0x08,0x10,0x10,0x10,0x08,0x04,0,0,0},
{0x10,0x08,0x04,0x04,0x04,0x08,0x10,0,0,0},
{0x08,0x2A,0x1C,0x08,0x1C,0x2A,0x08,0,0,0},
{0,0x08,0x08,0x3E,0x08,0x08,0,0,0,0},
{0,0,0,0,0,0x08,0x08,0x10,0,0},
{0,0,0,0x3E,0,0,0,0,0,0},
{0,0,0,0,0,0,0x08,0,0,0},
{0,0x02,0x04,0x08,0x10,0x20,0,0,0,0},
{0x1C,0x22,0x26,0x2A,0x32,0x22,0x1C,0,0,0},
{0x08,0x18,0x08,0x08,0x08,0x08,0x1C,0,0,0},
{0x1C,0x22,0x02,0x04,0x08,0x10,0x3E,0,0,0},
{0x1C,0x22,0x02,0x0C,0x02,0x22,0x1C,0,0,0},
{0x04,0x0C,0x14,0x24,0x3E,0x04,0x04,0,0,0},
{0x3E,0x20,0x3C,0x02,0x02,0x22,0x1C,0,0,0},
{0x0C,0x10,0x20,0x3C,0x22,0x22,0x1C,0,0,0},
{0x3E,0x02,0x04,0x08,0x10,0x10,0x10,0,0,0},
{0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C,0,0,0},
{0x1C,0x22,0x22,0x1E,0x02,0x04,0x18,0,0,0},
{0,0x08,0,0,0,0x08,0,0,0,0},
{0,0x08,0,0,0,0x08,0x08,0x10,0,0},
{0x04,0x08,0x10,0x20,0x10,0x08,0x04,0,0,0},
{0,0,0x3E,0,0x3E,0,0,0,0,0},
{0x10,0x08,0x04,0x02,0x04,0x08,0x10,0,0,0},
{0x1C,0x22,0x02,0x04,0x08,0,0x08,0,0,0},
{0x1C,0x22,0x2E,0x2A,0x2E,0x20,0x1C,0,0,0},
{0x08,0x14,0x22,0x22,0x3E,0x22,0x22,0,0,0},
{0x3C,0x22,0x22,0x3C,0x22,0x22,0x3C,0,0,0},
{0x1C,0x22,0x20,0x20,0x20,0x22,0x1C,0,0,0},
{0x38,0x24,0x22,0x22,0x22,0x24,0x38,0,0,0},
{0x3E,0x20,0x20,0x3C,0x20,0x20,0x3E,0,0,0},
{0x3E,0x20,0x20,0x3C,0x20,0x20,0x20,0,0,0},
{0x1C,0x22,0x20,0x2E,0x22,0x22,0x1C,0,0,0},
{0x22,0x22,0x22,0x3E,0x22,0x22,0x22,0,0,0},
{0x1C,0x08,0x08,0x08,0x08,0x08,0x1C,0,0,0},
{0x02,0x02,0x02,0x02,0x02,0x22,0x1C,0,0,0},
{0x22,0x24,0x28,0x30,0x28,0x24,0x22,0,0,0},
{0x20,0x20,0x20,0x20,0x20,0x20,0x3E,0,0,0},
{0x22,0x36,0x2A,0x2A,0x22,0x22,0x22,0,0,0},
{0x22,0x32,0x2A,0x26,0x22,0x22,0x22,0,0,0},
{0x1C,0x22,0x22,0x22,0x22,0x22,0x1C,0,0,0},
{0x3C,0x22,0x22,0x3C,0x20,0x20,0x20,0,0,0},
{0x1C,0x22,0x22,0x22,0x2A,0x24,0x1A,0,0,0},
{0x3C,0x22,0x22,0x3C,0x28,0x24,0x22,0,0,0},
{0x1C,0x22,0x20,0x1C,0x02,0x22,0x1C,0,0,0},
{0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0,0,0},
{0x22,0x22,0x22,0x22,0x22,0x22,0x1C,0,0,0},
{0x22,0x22,0x22,0x14,0x14,0x08,0x08,0,0,0},
{0x22,0x22,0x22,0x2A,0x2A,0x36,0x22,0,0,0},
{0x22,0x22,0x14,0x08,0x14,0x22,0x22,0,0,0},
{0x22,0x22,0x14,0x08,0x08,0x08,0x08,0,0,0},
{0x3E,0x02,0x04,0x08,0x10,0x20,0x3E,0,0,0},
{0x1C,0x10,0x10,0x10,0x10,0x10,0x1C,0,0,0},
{0,0x20,0x10,0x08,0x04,0x02,0,0,0,0},
{0x1C,0x04,0x04,0x04,0x04,0x04,0x1C,0,0,0},
{0x08,0x14,0x22,0,0,0,0,0,0,0},
{0,0,0,0,0,0,0x3E,0,0,0},
{0x10,0x08,0,0,0,0,0,0,0,0},
{0,0,0x1C,0x02,0x1E,0x22,0x1E,0,0,0},
{0x20,0x20,0x2C,0x32,0x22,0x32,0x2C,0,0,0},
{0,0,0x1C,0x20,0x20,0x20,0x1C,0,0,0},
{0x02,0x02,0x1A,0x26,0x22,0x26,0x1A,0,0,0},
{0,0,0x1C,0x22,0x3E,0x20,0x1C,0,0,0},
{0x0C,0x10,0x10,0x3C,0x10,0x10,0x10,0,0,0},
{0,0,0x1E,0x22,0x1E,0x02,0x1C,0,0,0},
{0x20,0x20,0x2C,0x32,0x22,0x22,0x22,0,0,0},
{0x08,0,0x18,0x08,0x08,0x08,0x1C,0,0,0},
{0x04,0,0x04,0x04,0x04,0x24,0x18,0,0,0},
{0x20,0x20,0x24,0x28,0x30,0x28,0x24,0,0,0},
{0x18,0x08,0x08,0x08,0x08,0x08,0x1C,0,0,0},
{0,0,0x36,0x2A,0x2A,0x22,0x22,0,0,0},
{0,0,0x2C,0x32,0x22,0x22,0x22,0,0,0},
{0,0,0x1C,0x22,0x22,0x22,0x1C,0,0,0},
{0,0,0x2C,0x32,0x3C,0x20,0x20,0,0,0},
{0,0,0x1A,0x26,0x1E,0x02,0x02,0,0,0},
{0,0,0x2C,0x32,0x20,0x20,0x20,0,0,0},
{0,0,0x1E,0x20,0x1C,0x02,0x3C,0,0,0},
{0x10,0x10,0x3C,0x10,0x10,0x12,0x0C,0,0,0},
{0,0,0x22,0x22,0x22,0x26,0x1A,0,0,0},
{0,0,0x22,0x22,0x14,0x14,0x08,0,0,0},
{0,0,0x22,0x22,0x2A,0x2A,0x14,0,0,0},
{0,0,0x22,0x14,0x08,0x14,0x22,0,0,0},
{0,0,0x22,0x22,0x1E,0x02,0x1C,0,0,0},
{0,0,0x3E,0x04,0x08,0x10,0x3E,0,0,0},
{0x0C,0x10,0x10,0x20,0x10,0x10,0x0C,0,0,0},
{0x08,0x08,0x08,0,0x08,0x08,0x08,0,0,0},
{0x18,0x04,0x04,0x02,0x04,0x04,0x18,0,0,0},
{0x14,0x28,0,0,0,0,0,0,0,0},
};

#define FW 6
#define FH 10
#define FS 2  /* scale factor: each pixel → 2×2 */
#define FCW (FW*FS+1)
#define FCH (FH*FS+2)

static void fb_putchar(int px,int py,char c,unsigned int col,int alpha){
    if(c<32||c>126) c=' ';
    const unsigned char *g=FONT[(unsigned char)c-32];
    for(int row=0;row<FH;row++){
        unsigned char bits=g[row];
        for(int cb=0;cb<FW;cb++){
            if(!((bits>>(5-cb))&1)) continue;
            for(int sy=0;sy<FS;sy++)
                for(int sx=0;sx<FS;sx++){
                    int dx=px+cb*FS+sx, dy=py+row*FS+sy;
                    fb_put(dx,dy,alpha==255?col:fb_blend(wp_get(dx,dy),col,alpha));
                }
        }
    }
}

static int fb_putstr(int px,int py,const char *s,unsigned int col,int alpha){
    int x=px;
    for(;*s;s++){
        if(*s=='\n'){x=px;py+=FCH;continue;}
        fb_putchar(x,py,*s,col,alpha);
        x+=FCW;
    }
    return x;
}

/* ── panel primitives ────────────────────────────────────── */
static void fb_fill(int px,int py,int pw,int ph,
                    unsigned int bg,unsigned int tbg,
                    unsigned int border,int alpha){
    for(int y=py;y<py+ph;y++)
        for(int x=px;x<px+pw;x++){
            unsigned int c=(y<py+36)?tbg:bg;
            fb_put(x,y,fb_blend(wp_get(x,y),c,alpha));
        }
    for(int x=px;x<px+pw;x++){
        fb_put(x,py,border);fb_put(x,py+1,border);
        fb_put(x,py+ph-1,border);fb_put(x,py+ph-2,border);
    }
    for(int y=py;y<py+ph;y++){
        fb_put(px,y,border);fb_put(px+1,y,border);
        fb_put(px+pw-1,y,border);fb_put(px+pw-2,y,border);
    }
    /* rounded corners */
    for(int dy=0;dy<12;dy++)
        for(int dx=0;dx<12-dy;dx++){
            fb_put(px+dx,py+dy,wp_get(px+dx,py+dy));
            fb_put(px+pw-1-dx,py+dy,wp_get(px+pw-1-dx,py+dy));
            fb_put(px+dx,py+ph-1-dy,wp_get(px+dx,py+ph-1-dy));
            fb_put(px+pw-1-dx,py+ph-1-dy,wp_get(px+pw-1-dx,py+ph-1-dy));
        }
}

static void fb_restore(int px,int py,int pw,int ph){
    for(int y=py;y<py+ph&&y<fb.h;y++)
        for(int x=px;x<px+pw&&x<fb.w;x++)
            fb_put(x,y,wp_get(x,y));
}

static void fb_hline(int px,int py,int pw,unsigned int col){
    for(int x=px;x<px+pw;x++) fb_put(x,py,col);
}

/* ── geometry ────────────────────────────────────────────── */
static void menu_rect(int *px,int *py,int *pw,int *ph){
    *pw=fb.w*48/100; *ph=fb.h*72/100;
    *px=(fb.w-*pw)/2; *py=(fb.h-*ph)/2;
}
static void term_rect(int *px,int *py,int *pw,int *ph){
    int m=fb.w*2/100;
    *px=m;*py=m;*pw=fb.w-m*2;*ph=fb.h-m*2;
}

/* ── menu ────────────────────────────────────────────────── */
static const struct{const char *label;const char *cmd;}MITEMS[]={
    {"  Snake",       "snake"},
    {"  Tetris",      "tetris"},
    {"  Pongy",       "pongy"},
    {"  Chicken",     "chicken"},
    {"  Calculator",  "calcui"},
    {"  Editor",      "edit /tmp/scratch.txt"},
    {"  Files",       "files"},
    {"  Web",         "web"},
    {"  Reboot",      "reboot"},
    {"  Poweroff",    "poweroff"},
};
#define MN 10
static int mn_sel=0;
static int menu_visible=0;

static int run_line(char *);  /* forward */

static void draw_menu(void){
    int px,py,pw,ph; menu_rect(&px,&py,&pw,&ph);
    fb_fill(px,py,pw,ph,COL_PANEL_BG,COL_TITLEBAR,COL_BORDER,PANEL_ALPHA);

    /* title */
    const char *title="TRIUMPH  OS";
    int tx=px+(pw-(int)strlen(title)*FCW)/2;
    fb_putstr(tx,py+8,title,COL_ACCENT,255);
    fb_hline(px+16,py+36,pw-32,COL_BORDER);

    /* items */
    int iy=py+46;
    for(int i=0;i<MN;i++,iy+=FCH+6){
        if(i==mn_sel){
            for(int bx=px+8;bx<px+pw-8;bx++)
                for(int by=iy-3;by<iy+FCH+3;by++)
                    fb_put(bx,by,fb_blend(wp_get(bx,by),COL_SEL_BG,200));
            fb_putstr(px+12,iy,MITEMS[i].label,COL_SEL_FG,255);
        } else {
            fb_putstr(px+12,iy,MITEMS[i].label,COL_TEXT,220);
        }
    }

    /* footer */
    const char *foot="Up/Down  Enter=launch  Shift+M=close";
    int fx=px+(pw-(int)strlen(foot)*FCW)/2;
    fb_hline(px+16,py+ph-FCH-16,pw-32,COL_DIM);
    fb_putstr(fx,py+ph-FCH-8,foot,COL_DIM,200);
}

/* ── terminal (pty) ──────────────────────────────────────── */
#define TROWS 35
#define TCOLS 100
static char tscreen[TROWS][TCOLS+1];
static int  tcur_row=0,tcur_col=0;
static int  term_pty_master=-1;
static pid_t term_child=-1;
static int  term_visible=0;
static pthread_mutex_t term_lock=PTHREAD_MUTEX_INITIALIZER;

static void tscroll(void){
    memmove(tscreen[0],tscreen[1],(TROWS-1)*sizeof(tscreen[0]));
    memset(tscreen[TROWS-1],0,sizeof(tscreen[0]));
    tcur_row=TROWS-1;
}

static void tputc(char c){
    if(c=='\n'||tcur_col>=TCOLS){
        tcur_col=0; tcur_row++;
        if(tcur_row>=TROWS) tscroll();
    }
    if(c=='\r'){tcur_col=0;return;}
    if(c=='\b'||c==127){if(tcur_col>0)tscreen[tcur_row][--tcur_col]=0;return;}
    if(c>=32&&c<127) tscreen[tcur_row][tcur_col++]=c;
}

/* strip ANSI escape sequences — we do our own rendering */
static void twrite(const char *buf,int n){
    pthread_mutex_lock(&term_lock);
    int i=0;
    while(i<n){
        if(buf[i]==0x1b){
            i++;
            if(i<n&&buf[i]=='['){
                i++;
                while(i<n&&(buf[i]==';'||
                      (buf[i]>='0'&&buf[i]<='9'))) i++;
                if(i<n) i++; /* skip final byte */
                continue;
            }
            continue;
        }
        tputc(buf[i++]);
    }
    pthread_mutex_unlock(&term_lock);
}

static void draw_term(void){
    int px,py,pw,ph; term_rect(&px,&py,&pw,&ph);
    fb_fill(px,py,pw,ph,COL_TERM_BG,COL_TITLEBAR,COL_TERM_BOR,TERM_ALPHA);

    /* title bar */
    fb_putstr(px+10,py+8,"  Triumph Terminal",COL_ACCENT,255);
    const char *hint="Shift+T to close";
    fb_putstr(px+pw-(int)strlen(hint)*FCW-10,py+8,hint,COL_DIM,200);
    fb_hline(px+6,py+36,pw-12,COL_TERM_BOR);

    /* content */
    pthread_mutex_lock(&term_lock);
    int ty=py+44;
    for(int r=0;r<TROWS&&ty+FCH<py+ph-8;r++,ty+=FCH+1){
        if(tscreen[r][0])
            fb_putstr(px+8,ty,tscreen[r],COL_TEXT,230);
    }
    /* cursor on last active row */
    int cx=px+8+tcur_col*FCW;
    int cy=py+44+tcur_row*(FCH+1);
    if(cy+FCH<py+ph-8)
        for(int yy=cy;yy<cy+FCH;yy++) fb_put(cx,yy,COL_ACCENT);
    pthread_mutex_unlock(&term_lock);
}

/* pty reader thread — reads shell output, strips ANSI, updates tscreen */
static void *pty_reader(void *arg){
    (void)arg;
    char buf[1024];
    while(1){
        int n=read(term_pty_master,buf,sizeof(buf));
        if(n<=0) break;
        twrite(buf,n);
        if(term_visible) draw_term();
    }
    return NULL;
}

static void term_open(void){
    if(term_pty_master>=0) return; /* already open */
    memset(tscreen,0,sizeof(tscreen));
    tcur_row=0; tcur_col=0;

    struct winsize ws={TROWS,TCOLS,0,0};
    term_child=forkpty(&term_pty_master,NULL,NULL,&ws);
    if(term_child==0){
        /* child: exec triumph shell */
        setenv("TERM","xterm-256color",1);
        char *av[]={"triumph",NULL};
        execv("/bin/triumph",av);
        execv("/bin/sh",av);
        _exit(1);
    }
    pthread_t t;
    pthread_create(&t,NULL,pty_reader,NULL);
    pthread_detach(t);
}

static void term_close(void){
    if(term_child>0){ kill(term_child,SIGTERM); waitpid(term_child,NULL,WNOHANG); term_child=-1; }
    if(term_pty_master>=0){ close(term_pty_master); term_pty_master=-1; }
}

/* send key to pty */
static void term_sendkey(int key){
    if(term_pty_master<0) return;
    char c=(char)key;
    write(term_pty_master,&c,1);
}

/* ── toggle handlers ─────────────────────────────────────── */
static void fb_toggle_menu(void){
    if(fb.fd<0) return;
    if(menu_visible){
        int px,py,pw,ph; menu_rect(&px,&py,&pw,&ph);
        fb_restore(px,py,pw,ph);
        menu_visible=0;
        if(term_visible) draw_term();
    } else {
        menu_visible=1;
        draw_menu();
    }
}

static void fb_toggle_term(void){
    if(fb.fd<0) return;
    if(term_visible){
        term_close();
        int px,py,pw,ph; term_rect(&px,&py,&pw,&ph);
        fb_restore(px,py,pw,ph);
        term_visible=0;
        if(menu_visible) draw_menu();
    } else {
        term_open();
        term_visible=1;
        draw_term();
    }
}

/* ── menu key handling ───────────────────────────────────── */
static void menu_key(int k){
    if(!menu_visible) return;
    if(k==0x101||k=='k'||k=='w'){ mn_sel=(mn_sel-1+MN)%MN; draw_menu(); return; }
    if(k==0x102||k=='j'||k=='s'){ mn_sel=(mn_sel+1)%MN;    draw_menu(); return; }
    if(k==13||k==10){
        /* launch — close menu, restore tty, run cmd, redraw wallpaper */
        int px,py,pw,ph; menu_rect(&px,&py,&pw,&ph);
        fb_restore(px,py,pw,ph);
        menu_visible=0;
        char cmd[256]; strncpy(cmd,(char*)MITEMS[mn_sel].cmd,255);
        run_line(cmd);
        /* after app exits, redraw wallpaper and any open panels */
        /* fb draw wallpaper */
        if(fb.wp){
            if(fb.bpp==32&&fb.r_off==16&&fb.g_off==8&&fb.b_off==0)
                for(int y=0;y<fb.h;y++)
                    memcpy(fb.mem+y*fb.stride,fb.wp+y*fb.w,(size_t)fb.w*4);
            else for(int y=0;y<fb.h;y++) for(int x=0;x<fb.w;x++) fb_put(x,y,fb.wp[y*fb.w+x]);
        }
        if(term_visible) draw_term();
    }
}

/* ── external keyboard ───────────────────────────────────── */
#define MAX_KBD 16
static int kbd_fds[MAX_KBD],kbd_nfds=0,kbd_shift=0;

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
                if(ev.code==KEY_LEFTSHIFT||ev.code==KEY_RIGHTSHIFT) kbd_shift=(ev.value!=0);
                if(ev.value==1){
                    if(kbd_shift&&ev.code==KEY_M){fb_toggle_menu();continue;}
                    if(kbd_shift&&ev.code==KEY_T){fb_toggle_term();continue;}
                    /* route other keys to terminal if open */
                    if(term_visible&&!kbd_shift){
                        /* convert keycode to ascii roughly */
                        static const char kmap[128]={
                            [KEY_A]='a',[KEY_B]='b',[KEY_C]='c',[KEY_D]='d',
                            [KEY_E]='e',[KEY_F]='f',[KEY_G]='g',[KEY_H]='h',
                            [KEY_I]='i',[KEY_J]='j',[KEY_K]='k',[KEY_L]='l',
                            [KEY_M]='m',[KEY_N]='n',[KEY_O]='o',[KEY_P]='p',
                            [KEY_Q]='q',[KEY_R]='r',[KEY_S]='s',[KEY_T]='t',
                            [KEY_U]='u',[KEY_V]='v',[KEY_W]='w',[KEY_X]='x',
                            [KEY_Y]='y',[KEY_Z]='z',
                            [KEY_1]='1',[KEY_2]='2',[KEY_3]='3',[KEY_4]='4',
                            [KEY_5]='5',[KEY_6]='6',[KEY_7]='7',[KEY_8]='8',
                            [KEY_9]='9',[KEY_0]='0',
                            [KEY_SPACE]=' ',[KEY_ENTER]='\n',[KEY_BACKSPACE]='\b',
                            [KEY_MINUS]='-',[KEY_EQUAL]='=',[KEY_DOT]='.',
                            [KEY_COMMA]=',',[KEY_SLASH]='/',[KEY_SEMICOLON]=';',
                        };
                        if(ev.code<128&&kmap[ev.code]) term_sendkey(kmap[ev.code]);
                    }
                    if(menu_visible) menu_key(ev.code==KEY_UP?0x101:ev.code==KEY_DOWN?0x102:
                                              ev.code==KEY_ENTER?13:0);
                }
            }
        }
    }
    return NULL;
}

/* ── TTY suppression ─────────────────────────────────────── */
static void suppress_tty(void){
    int fd=open("/dev/tty0",O_RDWR); if(fd<0) fd=open("/dev/console",O_RDWR);
    if(fd>=0){ioctl(fd,KDSETMODE,KD_GRAPHICS);close(fd);}
}
static void restore_tty(void){
    int fd=open("/dev/tty0",O_RDWR); if(fd<0) fd=open("/dev/console",O_RDWR);
    if(fd>=0){ioctl(fd,KDSETMODE,KD_TEXT);close(fd);}
}

/* ── fb init ─────────────────────────────────────────────── */
static int fb_init(void){
    const char *devs[]={"/dev/fb0","/dev/fb1","/dev/graphics/fb0",NULL};
    for(int i=0;devs[i];i++){fb.fd=open(devs[i],O_RDWR);if(fb.fd>=0)break;}
    if(fb.fd<0) return -1;
    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if(ioctl(fb.fd,FBIOGET_VSCREENINFO,&vi)<0||ioctl(fb.fd,FBIOGET_FSCREENINFO,&fi)<0)
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

static void fb_draw_wallpaper(void){
    if(fb.fd<0||!fb.wp) return;
    if(fb.bpp==32&&fb.r_off==16&&fb.g_off==8&&fb.b_off==0)
        for(int y=0;y<fb.h;y++) memcpy(fb.mem+y*fb.stride,fb.wp+y*fb.w,(size_t)fb.w*4);
    else for(int y=0;y<fb.h;y++) for(int x=0;x<fb.w;x++) fb_put(x,y,fb.wp[y*fb.w+x]);
}

/* ── startup / shutdown ──────────────────────────────────── */
static void fb_startup(void){
    if(fb_init()<0) return;
    suppress_tty();
    fb_draw_wallpaper();
    pthread_t t; pthread_create(&t,NULL,kbd_thread,NULL); pthread_detach(t);
}
static void fb_shutdown(void){
    if(fb.fd<0) return;
    term_close();
    restore_tty();
    free(fb.wp);fb.wp=NULL;
    munmap(fb.mem,fb.memsize);
    close(fb.fd);fb.fd=-1;
}
