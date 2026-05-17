/*
 * fb.c - Triumph OS framebuffer compositor
 *
 * ALL rendering goes through /dev/fb0 — TTY output is suppressed.
 * Boot → pure wallpaper.
 * Shift+M → transparent menu panel drawn in pixels over wallpaper.
 * Shift+T → transparent terminal panel drawn in pixels over wallpaper.
 * External USB/BT keyboards supported via /dev/input/event*.
 */

#pragma once

#include <linux/fb.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <sys/mman.h>
#include <pthread.h>
#include "wallpaper.h"

/* ── tuning ─────────────────────────────────────────────── */
#define PANEL_ALPHA   160   /* 0=invisible 255=opaque */
#define TERM_ALPHA    140
#define COL_PANEL_BG  0x060D1A
#define COL_TERM_BG   0x04080F
#define COL_TITLEBAR  0x0A1428
#define COL_BORDER    0x33CCFF
#define COL_TERM_BOR  0x1199DD
#define COL_TEXT      0xCCEEFF
#define COL_DIM       0x557799
#define COL_ACCENT    0x33CCFF
#define COL_GREEN     0x44FF88
#define COL_YELLOW    0xFFDD44

/* ── framebuffer ─────────────────────────────────────────── */
typedef struct {
    int fd, w, h, stride, bpp;
    int r_off, g_off, b_off;
    char *mem;
    size_t memsize;
    unsigned int *wp; /* pre-scaled wallpaper XRGB */
} FB;

static FB fb = { .fd = -1 };

/* ── pixel font (6×10, ASCII 32-126) ────────────────────── */
/* A minimal built-in bitmap font so we never need TTY rendering */
static const unsigned char FONT[95][10] = {
{0,0,0,0,0,0,0,0,0,0},       /* space */
{0x08,0x08,0x08,0x08,0x08,0,0x08,0,0,0},  /* ! */
{0x14,0x14,0,0,0,0,0,0,0,0}, /* " */
{0x14,0x14,0x3E,0x14,0x3E,0x14,0x14,0,0,0}, /* # */
{0x08,0x1E,0x28,0x1C,0x0A,0x3C,0x08,0,0,0}, /* $ */
{0x30,0x32,0x04,0x08,0x10,0x26,0x06,0,0,0}, /* % */
{0x10,0x28,0x28,0x10,0x2A,0x24,0x1A,0,0,0}, /* & */
{0x08,0x08,0,0,0,0,0,0,0,0}, /* ' */
{0x04,0x08,0x10,0x10,0x10,0x08,0x04,0,0,0}, /* ( */
{0x10,0x08,0x04,0x04,0x04,0x08,0x10,0,0,0}, /* ) */
{0x08,0x2A,0x1C,0x08,0x1C,0x2A,0x08,0,0,0}, /* * */
{0,0x08,0x08,0x3E,0x08,0x08,0,0,0,0}, /* + */
{0,0,0,0,0,0x08,0x08,0x10,0,0}, /* , */
{0,0,0,0x3E,0,0,0,0,0,0},   /* - */
{0,0,0,0,0,0,0x08,0,0,0},   /* . */
{0,0x02,0x04,0x08,0x10,0x20,0,0,0,0}, /* / */
{0x1C,0x22,0x26,0x2A,0x32,0x22,0x1C,0,0,0}, /* 0 */
{0x08,0x18,0x08,0x08,0x08,0x08,0x1C,0,0,0}, /* 1 */
{0x1C,0x22,0x02,0x04,0x08,0x10,0x3E,0,0,0}, /* 2 */
{0x1C,0x22,0x02,0x0C,0x02,0x22,0x1C,0,0,0}, /* 3 */
{0x04,0x0C,0x14,0x24,0x3E,0x04,0x04,0,0,0}, /* 4 */
{0x3E,0x20,0x3C,0x02,0x02,0x22,0x1C,0,0,0}, /* 5 */
{0x0C,0x10,0x20,0x3C,0x22,0x22,0x1C,0,0,0}, /* 6 */
{0x3E,0x02,0x04,0x08,0x10,0x10,0x10,0,0,0}, /* 7 */
{0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C,0,0,0}, /* 8 */
{0x1C,0x22,0x22,0x1E,0x02,0x04,0x18,0,0,0}, /* 9 */
{0,0x08,0,0,0,0x08,0,0,0,0}, /* : */
{0,0x08,0,0,0,0x08,0x08,0x10,0,0}, /* ; */
{0x04,0x08,0x10,0x20,0x10,0x08,0x04,0,0,0}, /* < */
{0,0,0x3E,0,0x3E,0,0,0,0,0}, /* = */
{0x10,0x08,0x04,0x02,0x04,0x08,0x10,0,0,0}, /* > */
{0x1C,0x22,0x02,0x04,0x08,0,0x08,0,0,0}, /* ? */
{0x1C,0x22,0x2E,0x2A,0x2E,0x20,0x1C,0,0,0}, /* @ */
{0x08,0x14,0x22,0x22,0x3E,0x22,0x22,0,0,0}, /* A */
{0x3C,0x22,0x22,0x3C,0x22,0x22,0x3C,0,0,0}, /* B */
{0x1C,0x22,0x20,0x20,0x20,0x22,0x1C,0,0,0}, /* C */
{0x38,0x24,0x22,0x22,0x22,0x24,0x38,0,0,0}, /* D */
{0x3E,0x20,0x20,0x3C,0x20,0x20,0x3E,0,0,0}, /* E */
{0x3E,0x20,0x20,0x3C,0x20,0x20,0x20,0,0,0}, /* F */
{0x1C,0x22,0x20,0x2E,0x22,0x22,0x1C,0,0,0}, /* G */
{0x22,0x22,0x22,0x3E,0x22,0x22,0x22,0,0,0}, /* H */
{0x1C,0x08,0x08,0x08,0x08,0x08,0x1C,0,0,0}, /* I */
{0x02,0x02,0x02,0x02,0x02,0x22,0x1C,0,0,0}, /* J */
{0x22,0x24,0x28,0x30,0x28,0x24,0x22,0,0,0}, /* K */
{0x20,0x20,0x20,0x20,0x20,0x20,0x3E,0,0,0}, /* L */
{0x22,0x36,0x2A,0x2A,0x22,0x22,0x22,0,0,0}, /* M */
{0x22,0x32,0x2A,0x26,0x22,0x22,0x22,0,0,0}, /* N */
{0x1C,0x22,0x22,0x22,0x22,0x22,0x1C,0,0,0}, /* O */
{0x3C,0x22,0x22,0x3C,0x20,0x20,0x20,0,0,0}, /* P */
{0x1C,0x22,0x22,0x22,0x2A,0x24,0x1A,0,0,0}, /* Q */
{0x3C,0x22,0x22,0x3C,0x28,0x24,0x22,0,0,0}, /* R */
{0x1C,0x22,0x20,0x1C,0x02,0x22,0x1C,0,0,0}, /* S */
{0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0,0,0}, /* T */
{0x22,0x22,0x22,0x22,0x22,0x22,0x1C,0,0,0}, /* U */
{0x22,0x22,0x22,0x14,0x14,0x08,0x08,0,0,0}, /* V */
{0x22,0x22,0x22,0x2A,0x2A,0x36,0x22,0,0,0}, /* W */
{0x22,0x22,0x14,0x08,0x14,0x22,0x22,0,0,0}, /* X */
{0x22,0x22,0x14,0x08,0x08,0x08,0x08,0,0,0}, /* Y */
{0x3E,0x02,0x04,0x08,0x10,0x20,0x3E,0,0,0}, /* Z */
{0x1C,0x10,0x10,0x10,0x10,0x10,0x1C,0,0,0}, /* [ */
{0,0x20,0x10,0x08,0x04,0x02,0,0,0,0}, /* \ */
{0x1C,0x04,0x04,0x04,0x04,0x04,0x1C,0,0,0}, /* ] */
{0x08,0x14,0x22,0,0,0,0,0,0,0}, /* ^ */
{0,0,0,0,0,0,0x3E,0,0,0},   /* _ */
{0x10,0x08,0,0,0,0,0,0,0,0}, /* ` */
{0,0,0x1C,0x02,0x1E,0x22,0x1E,0,0,0}, /* a */
{0x20,0x20,0x2C,0x32,0x22,0x32,0x2C,0,0,0}, /* b */
{0,0,0x1C,0x20,0x20,0x20,0x1C,0,0,0}, /* c */
{0x02,0x02,0x1A,0x26,0x22,0x26,0x1A,0,0,0}, /* d */
{0,0,0x1C,0x22,0x3E,0x20,0x1C,0,0,0}, /* e */
{0x0C,0x10,0x10,0x3C,0x10,0x10,0x10,0,0,0}, /* f */
{0,0,0x1E,0x22,0x1E,0x02,0x1C,0,0,0}, /* g */
{0x20,0x20,0x2C,0x32,0x22,0x22,0x22,0,0,0}, /* h */
{0x08,0,0x18,0x08,0x08,0x08,0x1C,0,0,0}, /* i */
{0x04,0,0x04,0x04,0x04,0x24,0x18,0,0,0}, /* j */
{0x20,0x20,0x24,0x28,0x30,0x28,0x24,0,0,0}, /* k */
{0x18,0x08,0x08,0x08,0x08,0x08,0x1C,0,0,0}, /* l */
{0,0,0x36,0x2A,0x2A,0x22,0x22,0,0,0}, /* m */
{0,0,0x2C,0x32,0x22,0x22,0x22,0,0,0}, /* n */
{0,0,0x1C,0x22,0x22,0x22,0x1C,0,0,0}, /* o */
{0,0,0x2C,0x32,0x3C,0x20,0x20,0,0,0}, /* p */
{0,0,0x1A,0x26,0x1E,0x02,0x02,0,0,0}, /* q */
{0,0,0x2C,0x32,0x20,0x20,0x20,0,0,0}, /* r */
{0,0,0x1E,0x20,0x1C,0x02,0x3C,0,0,0}, /* s */
{0x10,0x10,0x3C,0x10,0x10,0x12,0x0C,0,0,0}, /* t */
{0,0,0x22,0x22,0x22,0x26,0x1A,0,0,0}, /* u */
{0,0,0x22,0x22,0x14,0x14,0x08,0,0,0}, /* v */
{0,0,0x22,0x22,0x2A,0x2A,0x14,0,0,0}, /* w */
{0,0,0x22,0x14,0x08,0x14,0x22,0,0,0}, /* x */
{0,0,0x22,0x22,0x1E,0x02,0x1C,0,0,0}, /* y */
{0,0,0x3E,0x04,0x08,0x10,0x3E,0,0,0}, /* z */
{0x0C,0x10,0x10,0x20,0x10,0x10,0x0C,0,0,0}, /* { */
{0x08,0x08,0x08,0,0x08,0x08,0x08,0,0,0}, /* | */
{0x18,0x04,0x04,0x02,0x04,0x04,0x18,0,0,0}, /* } */
{0x14,0x28,0,0,0,0,0,0,0,0}, /* ~ */
};

#define FONT_W 6
#define FONT_H 10
#define FONT_SCALE 2  /* 2× scale = 12×20 pixels per char */

/* ── framebuffer pixel ops ───────────────────────────────── */

static inline unsigned int fb_blend(unsigned int bg, unsigned int fg, int a) {
    unsigned int rb  = bg&0xFF00FF, g_  = bg&0x00FF00;
    unsigned int rb2 = fg&0xFF00FF, g2  = fg&0x00FF00;
    return (((rb*(256-a)+rb2*a)>>8)&0xFF00FF)|(((g_*(256-a)+g2*a)>>8)&0x00FF00);
}

static inline void fb_put(int x, int y, unsigned int rgb) {
    if ((unsigned)x>=(unsigned)fb.w||(unsigned)y>=(unsigned)fb.h) return;
    unsigned int r=(rgb>>16)&0xff, g=(rgb>>8)&0xff, b=rgb&0xff;
    if (fb.bpp==32)
        *(unsigned int*)(fb.mem+y*fb.stride+x*4)=(r<<fb.r_off)|(g<<fb.g_off)|(b<<fb.b_off);
    else if (fb.bpp==16)
        *(unsigned short*)(fb.mem+y*fb.stride+x*2)=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
}

static inline unsigned int wp_get(int x, int y) {
    if (!fb.wp||(unsigned)x>=(unsigned)fb.w||(unsigned)y>=(unsigned)fb.h) return 0;
    return fb.wp[y*fb.w+x];
}

/* draw char to framebuffer at pixel pos (px,py) with colour col blended over wallpaper */
static void fb_putchar(int px, int py, char c, unsigned int col, int alpha) {
    if (c<32||c>126) c=32;
    const unsigned char *glyph = FONT[(unsigned char)c-32];
    for (int row=0; row<FONT_H; row++) {
        unsigned char bits = glyph[row];
        for (int col_bit=0; col_bit<FONT_W; col_bit++) {
            int on = (bits>>(5-col_bit))&1;
            if (!on) continue;
            for (int sy=0; sy<FONT_SCALE; sy++)
                for (int sx=0; sx<FONT_SCALE; sx++) {
                    int dx=px+col_bit*FONT_SCALE+sx;
                    int dy=py+row*FONT_SCALE+sy;
                    fb_put(dx,dy,fb_blend(wp_get(dx,dy),col,alpha));
                }
        }
    }
}

static void fb_putstr(int px, int py, const char *s,
                      unsigned int col, int alpha) {
    int x=px;
    for (; *s; s++) {
        if (*s=='\n') { x=px; py+=FONT_H*FONT_SCALE+2; continue; }
        fb_putchar(x,py,*s,col,alpha);
        x+=FONT_W*FONT_SCALE+1;
    }
}

/* ── panel drawing ───────────────────────────────────────── */

static void fb_fill_panel(int px, int py, int pw, int ph,
                           unsigned int bg, unsigned int title_bg,
                           unsigned int border_col, int alpha) {
    /* body */
    for (int y=py; y<py+ph; y++)
        for (int x=px; x<px+pw; x++) {
            unsigned int c=(y<py+36)?title_bg:bg;
            fb_put(x,y,fb_blend(wp_get(x,y),c,alpha));
        }
    /* border 2px */
    for (int x=px; x<px+pw; x++) {
        fb_put(x,py,border_col);fb_put(x,py+1,border_col);
        fb_put(x,py+ph-1,border_col);fb_put(x,py+ph-2,border_col);
    }
    for (int y=py; y<py+ph; y++) {
        fb_put(px,y,border_col);fb_put(px+1,y,border_col);
        fb_put(px+pw-1,y,border_col);fb_put(px+pw-2,y,border_col);
    }
    /* rounded corners r=12 */
    for (int dy=0;dy<12;dy++)
        for (int dx=0;dx<12-dy;dx++) {
            fb_put(px+dx,py+dy,wp_get(px+dx,py+dy));
            fb_put(px+pw-1-dx,py+dy,wp_get(px+pw-1-dx,py+dy));
            fb_put(px+dx,py+ph-1-dy,wp_get(px+dx,py+ph-1-dy));
            fb_put(px+pw-1-dx,py+ph-1-dy,wp_get(px+pw-1-dx,py+ph-1-dy));
        }
}

static void fb_restore_rect(int px, int py, int pw, int ph) {
    for (int y=py;y<py+ph&&y<fb.h;y++)
        for (int x=px;x<px+pw&&x<fb.w;x++)
            fb_put(x,y,wp_get(x,y));
}

/* ── geometry ────────────────────────────────────────────── */
static void menu_rect(int *px,int *py,int *pw,int *ph) {
    *pw=fb.w*52/100; *ph=fb.h*70/100;
    *px=(fb.w-*pw)/2; *py=(fb.h-*ph)/2;
}
static void term_rect(int *px,int *py,int *pw,int *ph) {
    int m=fb.w*2/100;
    *px=m; *py=m; *pw=fb.w-m*2; *ph=fb.h-m*2;
}

/* ── menu content ────────────────────────────────────────── */
static const struct { const char *label; const char *cmd; } MENU_ITEMS[] = {
    {"Snake",       "snake"},
    {"Tetris",      "tetris"},
    {"Pongy",       "pongy"},
    {"Chicken",     "chicken"},
    {"Calculator",  "calc"},
    {"Editor",      "edit"},
    {"Files",       "files"},
    {"Web",         "web"},
    {"Reboot",      "reboot"},
    {"Poweroff",    "poweroff"},
};
#define MENU_N 10
static int menu_sel = 0;

/* forward decl */
static void run_line(const char *);

static void fb_draw_menu_content(int px, int py, int pw, int ph) {
    /* title */
    int cw = FONT_W*FONT_SCALE+1;
    int ch = FONT_H*FONT_SCALE+2;
    const char *title = "TRIUMPH OS";
    int tx = px + (pw-(int)strlen(title)*cw)/2;
    fb_putstr(tx, py+10, title, COL_ACCENT, 255);

    /* divider */
    for (int x=px+20;x<px+pw-20;x++)
        fb_put(x,py+38,COL_BORDER);

    /* menu items */
    int iy = py+50;
    for (int i=0;i<MENU_N;i++) {
        int ix = px+(pw-(int)strlen(MENU_ITEMS[i].label)*cw)/2;
        unsigned int col = (i==menu_sel)?COL_ACCENT:COL_TEXT;
        int alpha = (i==menu_sel)?255:200;
        if (i==menu_sel) {
            /* highlight bar */
            for (int bx=px+10;bx<px+pw-10;bx++)
                for (int by=iy-2;by<iy+ch+2;by++)
                    fb_put(bx,by,fb_blend(wp_get(bx,by),COL_BORDER,40));
        }
        fb_putstr(ix,iy,MENU_ITEMS[i].label,col,alpha);
        iy+=ch+4;
    }

    /* hint */
    const char *hint = "ENTER select  ESC close";
    int hx=px+(pw-(int)strlen(hint)*cw)/2;
    fb_putstr(hx,py+ph-ch-10,hint,COL_DIM,200);
}

/* ── terminal content ────────────────────────────────────── */
#define TERM_COLS 120
#define TERM_ROWS 40
#define TERM_BUF  4096

static char term_lines[TERM_ROWS][TERM_COLS+1];
static int  term_row = 0;
static int  term_col = 0;
static char term_input[512];
static int  term_input_len = 0;
static int  term_cursor = 0;

static void term_newline(void) {
    term_col=0;
    term_row++;
    if (term_row>=TERM_ROWS) {
        memmove(term_lines[0],term_lines[1],(TERM_ROWS-1)*sizeof(term_lines[0]));
        memset(term_lines[TERM_ROWS-1],0,sizeof(term_lines[0]));
        term_row=TERM_ROWS-1;
    }
}

static void term_putc(char c) {
    if (c=='\n') { term_newline(); return; }
    if (c=='\r') { term_col=0; return; }
    if (c=='\b'||c==127) {
        if (term_col>0) { term_col--; term_lines[term_row][term_col]=0; }
        return;
    }
    if (term_col>=TERM_COLS) term_newline();
    term_lines[term_row][term_col++]=(c>=32&&c<127)?c:' ';
}

static void term_puts(const char *s) { for(;*s;s++) term_putc(*s); }

static void fb_draw_term_content(int px, int py, int pw, int ph) {
    int cw=FONT_W*FONT_SCALE+1, ch=FONT_H*FONT_SCALE+2;

    /* title bar */
    const char *title="  Triumph Terminal";
    fb_putstr(px+8,py+8,title,COL_ACCENT,255);

    /* close hint */
    const char *hint="Shift+T close";
    fb_putstr(px+pw-(int)strlen(hint)*cw-8,py+8,hint,COL_DIM,200);

    /* divider */
    for (int x=px+4;x<px+pw-4;x++) fb_put(x,py+36,COL_TERM_BOR);

    /* terminal lines */
    int ty=py+44;
    for (int r=0;r<TERM_ROWS&&ty+ch<py+ph-ch-16;r++,ty+=ch) {
        if (term_lines[r][0])
            fb_putstr(px+10,ty,term_lines[r],COL_TEXT,230);
    }

    /* input line */
    char prompt[TERM_COLS];
    snprintf(prompt,sizeof(prompt),"$ %s",term_input);
    int iy=py+ph-ch-10;
    for (int x=px+4;x<px+pw-4;x++) fb_put(x,iy-4,COL_TERM_BOR);
    fb_putstr(px+10,iy,prompt,COL_GREEN,255);

    /* cursor */
    int cx=px+10+(2+term_input_len)*cw;
    for (int y2=iy;y2<iy+ch;y2++)
        fb_put(cx,y2,COL_ACCENT);
}

/* ── overlay state ───────────────────────────────────────── */
static int menu_visible=0, term_visible=0;

static void redraw_menu(void) {
    int px,py,pw,ph; menu_rect(&px,&py,&pw,&ph);
    fb_fill_panel(px,py,pw,ph,COL_PANEL_BG,COL_TITLEBAR,COL_BORDER,PANEL_ALPHA);
    fb_draw_menu_content(px,py,pw,ph);
}

static void redraw_term(void) {
    int px,py,pw,ph; term_rect(&px,&py,&pw,&ph);
    fb_fill_panel(px,py,pw,ph,COL_TERM_BG,COL_TITLEBAR,COL_TERM_BOR,TERM_ALPHA);
    fb_draw_term_content(px,py,pw,ph);
}

static void fb_toggle_menu(void) {
    if (fb.fd<0) return;
    if (menu_visible) {
        int px,py,pw,ph; menu_rect(&px,&py,&pw,&ph);
        fb_restore_rect(px,py,pw,ph);
        menu_visible=0;
        if (term_visible) redraw_term();
    } else {
        menu_visible=1;
        redraw_menu();
    }
}

static void fb_toggle_term(void) {
    if (fb.fd<0) return;
    if (term_visible) {
        int px,py,pw,ph; term_rect(&px,&py,&pw,&ph);
        fb_restore_rect(px,py,pw,ph);
        term_visible=0;
        if (menu_visible) redraw_menu();
    } else {
        term_visible=1;
        redraw_term();
    }
}

/* ── fb init ─────────────────────────────────────────────── */
static int fb_init(void) {
    const char *devs[]={"/dev/fb0","/dev/fb1","/dev/graphics/fb0",NULL};
    for (int i=0;devs[i];i++){fb.fd=open(devs[i],O_RDWR);if(fb.fd>=0)break;}
    if (fb.fd<0) return -1;

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fb.fd,FBIOGET_VSCREENINFO,&vi)<0||
        ioctl(fb.fd,FBIOGET_FSCREENINFO,&fi)<0)
        {close(fb.fd);fb.fd=-1;return -1;}

    if (vi.bits_per_pixel!=32) {
        vi.bits_per_pixel=32;
        ioctl(fb.fd,FBIOPUT_VSCREENINFO,&vi);
        ioctl(fb.fd,FBIOGET_VSCREENINFO,&vi);
        ioctl(fb.fd,FBIOGET_FSCREENINFO,&fi);
    }

    fb.w=vi.xres; fb.h=vi.yres; fb.bpp=vi.bits_per_pixel;
    fb.stride=fi.line_length; fb.memsize=(size_t)fi.line_length*vi.yres;
    fb.r_off=vi.red.offset; fb.g_off=vi.green.offset; fb.b_off=vi.blue.offset;

    fb.mem=mmap(NULL,fb.memsize,PROT_READ|PROT_WRITE,MAP_SHARED,fb.fd,0);
    if (fb.mem==MAP_FAILED){close(fb.fd);fb.fd=-1;return -1;}

    fb.wp=malloc(sizeof(unsigned int)*(size_t)fb.w*fb.h);
    if (fb.wp) {
        for (int y=0;y<fb.h;y++){
            int sy=y*WP_H/fb.h; if(sy>=WP_H)sy=WP_H-1;
            for (int x=0;x<fb.w;x++){
                int sx=x*WP_W/fb.w; if(sx>=WP_W)sx=WP_W-1;
                fb.wp[y*fb.w+x]=wp_data[sy*WP_W+sx];
            }
        }
    }
    return 0;
}

static void fb_draw_wallpaper(void) {
    if (fb.fd<0||!fb.wp) return;
    if (fb.bpp==32&&fb.r_off==16&&fb.g_off==8&&fb.b_off==0) {
        for (int y=0;y<fb.h;y++)
            memcpy(fb.mem+y*fb.stride,fb.wp+y*fb.w,(size_t)fb.w*4);
    } else {
        for (int y=0;y<fb.h;y++)
            for (int x=0;x<fb.w;x++)
                fb_put(x,y,fb.wp[y*fb.w+x]);
    }
}

/* ── external keyboard thread ────────────────────────────── */
#define MAX_KBD 16
static int kbd_fds[MAX_KBD], kbd_nfds=0, kbd_shift=0;

static void kbd_scan(void) {
    for (int i=0;i<kbd_nfds;i++) close(kbd_fds[i]);
    kbd_nfds=0;
    char path[64];
    for (int i=0;i<32&&kbd_nfds<MAX_KBD;i++) {
        snprintf(path,sizeof(path),"/dev/input/event%d",i);
        int fd=open(path,O_RDONLY|O_NONBLOCK);
        if (fd<0) continue;
        unsigned long evbit=0;
        ioctl(fd,EVIOCGBIT(0,sizeof(evbit)),&evbit);
        if ((evbit>>EV_KEY)&1) kbd_fds[kbd_nfds++]=fd;
        else close(fd);
    }
}

static void *kbd_thread(void *arg) {
    (void)arg;
    kbd_scan();
    struct input_event ev;
    while (1) {
        fd_set fds; FD_ZERO(&fds);
        int maxfd=0;
        for (int i=0;i<kbd_nfds;i++){FD_SET(kbd_fds[i],&fds);if(kbd_fds[i]>maxfd)maxfd=kbd_fds[i];}
        struct timeval tv={5,0};
        if (select(maxfd+1,&fds,NULL,NULL,&tv)<=0){kbd_scan();continue;}
        for (int i=0;i<kbd_nfds;i++){
            if (!FD_ISSET(kbd_fds[i],&fds)) continue;
            while (read(kbd_fds[i],&ev,sizeof(ev))==sizeof(ev)){
                if (ev.type!=EV_KEY) continue;
                if (ev.code==KEY_LEFTSHIFT||ev.code==KEY_RIGHTSHIFT)
                    kbd_shift=(ev.value!=0);
                if (ev.value==1&&kbd_shift){
                    if (ev.code==KEY_M) fb_toggle_menu();
                    if (ev.code==KEY_T) fb_toggle_term();
                }
            }
        }
    }
    return NULL;
}

/* ── suppress TTY so framebuffer shows through ───────────── */
static void suppress_tty(void) {
    /* switch to graphics mode — blanks the TTY text layer */
    int ttyfd=open("/dev/tty0",O_RDWR);
    if (ttyfd<0) ttyfd=open("/dev/console",O_RDWR);
    if (ttyfd>=0) {
        ioctl(ttyfd,KDSETMODE,KD_GRAPHICS);
        close(ttyfd);
    }
}

static void restore_tty(void) {
    int ttyfd=open("/dev/tty0",O_RDWR);
    if (ttyfd<0) ttyfd=open("/dev/console",O_RDWR);
    if (ttyfd>=0) {
        ioctl(ttyfd,KDSETMODE,KD_TEXT);
        close(ttyfd);
    }
}

/* ── startup / shutdown ──────────────────────────────────── */
static void fb_startup(void) {
    if (fb_init()<0) return;
    suppress_tty();
    fb_draw_wallpaper();
    pthread_t t;
    pthread_create(&t,NULL,kbd_thread,NULL);
    pthread_detach(t);
}

static void fb_shutdown(void) {
    if (fb.fd<0) return;
    restore_tty();
    free(fb.wp); fb.wp=NULL;
    munmap(fb.mem,fb.memsize);
    close(fb.fd); fb.fd=-1;
}
