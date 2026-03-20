/*
 * tetris.c — Triumph OS built-in Tetris (clean rewrite)
 * Controls: ← → move, ↑/Z rotate, ↓ soft drop, Space hard drop, ^P pause, ^X quit
 */

#include <sys/time.h>

#define TB_W   10
#define TB_H   20

/* Each piece: 4 rotations x 4 cells x (row,col) */
static const int PIECES[7][4][4][2] = {
    /* I */{{{1,0},{1,1},{1,2},{1,3}},{{0,2},{1,2},{2,2},{3,2}},{{2,0},{2,1},{2,2},{2,3}},{{0,1},{1,1},{2,1},{3,1}}},
    /* O */{{{0,0},{0,1},{1,0},{1,1}},{{0,0},{0,1},{1,0},{1,1}},{{0,0},{0,1},{1,0},{1,1}},{{0,0},{0,1},{1,0},{1,1}}},
    /* T */{{{0,1},{1,0},{1,1},{1,2}},{{0,1},{1,1},{1,2},{2,1}},{{1,0},{1,1},{1,2},{2,1}},{{0,1},{1,0},{1,1},{2,1}}},
    /* S */{{{0,1},{0,2},{1,0},{1,1}},{{0,1},{1,1},{1,2},{2,2}},{{1,1},{1,2},{2,0},{2,1}},{{0,0},{1,0},{1,1},{2,1}}},
    /* Z */{{{0,0},{0,1},{1,1},{1,2}},{{0,2},{1,1},{1,2},{2,1}},{{1,0},{1,1},{2,1},{2,2}},{{0,1},{1,0},{1,1},{2,0}}},
    /* J */{{{0,0},{1,0},{1,1},{1,2}},{{0,1},{0,2},{1,1},{2,1}},{{1,0},{1,1},{1,2},{2,2}},{{0,1},{1,1},{2,0},{2,1}}},
    /* L */{{{0,2},{1,0},{1,1},{1,2}},{{0,1},{1,1},{2,1},{2,2}},{{1,0},{1,1},{1,2},{2,0}},{{0,0},{0,1},{1,1},{2,1}}},
};

/* ANSI bg colours per piece */
static const char *PC[7] = {
    "\x1b[46m",   /* I cyan    */
    "\x1b[43m",   /* O yellow  */
    "\x1b[45m",   /* T magenta */
    "\x1b[42m",   /* S green   */
    "\x1b[41m",   /* Z red     */
    "\x1b[44m",   /* J blue    */
    "\x1b[48;5;208m", /* L orange */
};
static const char *GHOST_C = "\x1b[48;5;236m";
static const char *RST_C   = "\x1b[0m";
static const char *CELL    = "  ";   /* two spaces = one cell */

typedef struct {
    int  board[TB_H][TB_W]; /* 0=empty, 1-7=piece+1 */
    int  pr, pc;            /* piece row, col */
    int  ptype, prot;
    int  ntype;             /* next type */
    int  score, level, lines;
    int  paused, dead;
    int  tcols, trows;      /* terminal dims */
} TB;

/* ── helpers ─────────────────────────────────────────────────────────────── */
static void tb_wr(const char *s) { write(1, s, strlen(s)); }
static void tb_xy(int x, int y)  { char b[24]; snprintf(b,24,"\x1b[%d;%dH",y,x); tb_wr(b); }

static int tb_valid(TB *t, int pr, int pc2, int pt, int rot) {
    for (int i = 0; i < 4; i++) {
        int r = pr  + PIECES[pt][rot][i][0];
        int c = pc2 + PIECES[pt][rot][i][1];
        if (c < 0 || c >= TB_W || r >= TB_H) return 0;
        if (r >= 0 && t->board[r][c])        return 0;
    }
    return 1;
}

static void tb_lock(TB *t) {
    for (int i = 0; i < 4; i++) {
        int r = t->pr + PIECES[t->ptype][t->prot][i][0];
        int c = t->pc + PIECES[t->ptype][t->prot][i][1];
        if (r >= 0 && r < TB_H && c >= 0 && c < TB_W)
            t->board[r][c] = t->ptype + 1;
    }
}

static void tb_clear(TB *t) {
    int cleared = 0;
    for (int r = TB_H-1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < TB_W; c++) if (!t->board[r][c]) { full=0; break; }
        if (full) {
            memmove(&t->board[1], &t->board[0], r * sizeof(t->board[0]));
            memset(t->board[0], 0, sizeof(t->board[0]));
            cleared++; r++;
        }
    }
    if (cleared) {
        static const int pts[] = {0,100,300,500,800};
        t->score += pts[cleared] * t->level;
        t->lines += cleared;
        t->level  = t->lines / 10 + 1;
    }
}

static void tb_spawn(TB *t) {
    t->ptype = t->ntype;
    t->prot  = 0;
    t->pr    = 0;
    t->pc    = TB_W/2 - 2;
    t->ntype = rand() % 7;
    if (!tb_valid(t, t->pr, t->pc, t->ptype, t->prot))
        t->dead = 1;
}

/* ── rendering ───────────────────────────────────────────────────────────── */
static void tb_draw(TB *t) {
    /* board origin on screen (1-based) */
    int ox = (t->tcols - TB_W*2 - 12) / 2; if (ox < 2) ox = 2;
    int oy = (t->trows - TB_H  -  2)  / 2; if (oy < 2) oy = 2;

    /* ── header ── */
    tb_xy(ox, oy-1);
    printf("\x1b[1m\x1b[38;5;51m Triumph Tetris \x1b[0m"
           "\x1b[38;5;245m Score:\x1b[38;5;226m\x1b[1m%-6d\x1b[0m"
           "\x1b[38;5;245m Lv:\x1b[38;5;82m\x1b[1m%-2d\x1b[0m"
           "\x1b[38;5;245m  \x1b[2m^X quit  ^P pause\x1b[0m",
           t->score, t->level);

    /* ── border ── */
    tb_xy(ox-1, oy);
    tb_wr("\x1b[38;5;240m╔");
    for (int c=0;c<TB_W;c++) tb_wr("══");
    tb_wr("╗");
    for (int r=0;r<TB_H;r++) {
        tb_xy(ox-1, oy+1+r);
        tb_wr("║\x1b[0m");
        for (int c=0;c<TB_W;c++) {
            int v = t->board[r][c];
            if (v) { tb_wr(PC[v-1]); tb_wr(CELL); tb_wr(RST_C); }
            else   tb_wr("  ");
        }
        tb_wr("\x1b[38;5;240m║\x1b[0m");
    }
    tb_xy(ox-1, oy+TB_H+1);
    tb_wr("\x1b[38;5;240m╚");
    for (int c=0;c<TB_W;c++) tb_wr("══");
    tb_wr("╝\x1b[0m");

    /* ── ghost piece ── */
    int gr = t->pr;
    while (tb_valid(t, gr+1, t->pc, t->ptype, t->prot)) gr++;
    if (gr != t->pr) {
        for (int i=0;i<4;i++) {
            int r = gr    + PIECES[t->ptype][t->prot][i][0];
            int c = t->pc + PIECES[t->ptype][t->prot][i][1];
            if (r>=0 && r<TB_H && !t->board[r][c]) {
                tb_xy(ox + c*2, oy+1+r);
                tb_wr(GHOST_C); tb_wr(CELL); tb_wr(RST_C);
            }
        }
    }

    /* ── active piece ── */
    for (int i=0;i<4;i++) {
        int r = t->pr + PIECES[t->ptype][t->prot][i][0];
        int c = t->pc + PIECES[t->ptype][t->prot][i][1];
        if (r >= 0 && r < TB_H) {
            tb_xy(ox + c*2, oy+1+r);
            tb_wr(PC[t->ptype]); tb_wr("\x1b[1m"); tb_wr(CELL); tb_wr(RST_C);
        }
    }

    /* ── next piece panel ── */
    int nx = ox + TB_W*2 + 3;
    tb_xy(nx, oy+1); tb_wr("\x1b[38;5;245mNEXT\x1b[0m");
    tb_xy(nx, oy+2); tb_wr("        ");
    tb_xy(nx, oy+3); tb_wr("        ");
    tb_xy(nx, oy+4); tb_wr("        ");
    tb_xy(nx, oy+5); tb_wr("        ");
    for (int i=0;i<4;i++) {
        int r = PIECES[t->ntype][0][i][0];
        int c = PIECES[t->ntype][0][i][1];
        tb_xy(nx + c*2, oy+2+r);
        tb_wr(PC[t->ntype]); tb_wr(CELL); tb_wr(RST_C);
    }
    tb_xy(nx, oy+7);
    printf("\x1b[38;5;245mLines:\x1b[38;5;226m\x1b[1m%d\x1b[0m  ", t->lines);

    /* ── pause overlay ── */
    if (t->paused) {
        tb_xy(ox + TB_W - 3, oy + TB_H/2);
        tb_wr("\x1b[1m\x1b[38;5;226m  PAUSED  \x1b[0m");
    }
    fflush(stdout);
}

static void tb_draw_over(TB *t) {
    int ox = (t->tcols - TB_W*2 - 12) / 2; if (ox < 2) ox = 2;
    int oy = (t->trows - TB_H  -  2)  / 2; if (oy < 2) oy = 2;
    int mx = ox + TB_W - 5;
    tb_xy(mx, oy+TB_H/2-1); tb_wr("\x1b[1m\x1b[38;5;196m   GAME OVER   \x1b[0m");
    tb_xy(mx, oy+TB_H/2+1); printf("\x1b[38;5;245m   Score: \x1b[38;5;226m\x1b[1m%d\x1b[0m   ", t->score);
    tb_xy(mx, oy+TB_H/2+2); tb_wr("\x1b[38;5;245m   Any key...   \x1b[0m");
    fflush(stdout);
}

/* ── input (non-blocking, 50ms poll) ────────────────────────────────────── */
static int tb_key(void) {
    fd_set fds; struct timeval tv={0,50000};
    FD_ZERO(&fds); FD_SET(0,&fds);
    if (select(1,&fds,NULL,NULL,&tv)<=0) return -1;
    unsigned char c; if(read(0,&c,1)!=1) return -1;
    if (c==27) {
        unsigned char s[2];
        struct timeval tv2={0,30000}; FD_ZERO(&fds); FD_SET(0,&fds);
        if(select(1,&fds,NULL,NULL,&tv2)<=0) return 27;
        if(read(0,&s[0],1)!=1) return 27;
        if(s[0]!='[') return 27;
        if(read(0,&s[1],1)!=1) return 27;
        switch(s[1]){case 'A':return 2001;case 'B':return 2002;case 'C':return 2003;case 'D':return 2004;}
        return 27;
    }
    return c;
}

/* ── main entry ─────────────────────────────────────────────────────────── */
static int b_tetris(Cmd *cmd) {
    (void)cmd;
    struct termios saved; term_raw(); tcgetattr(0,&saved);
    struct winsize ws; int tc=80,tr=24;
    if(ioctl(1,TIOCGWINSZ,&ws)==0&&ws.ws_col&&ws.ws_row){tc=ws.ws_col;tr=ws.ws_row;}
    srand((unsigned)time(NULL));
    tb_wr("\x1b[2J\x1b[H\x1b[?25l");

    TB t; memset(&t,0,sizeof(t));
    t.tcols=tc; t.trows=tr; t.level=1;
    t.ntype=rand()%7;
    tb_spawn(&t);

    struct timeval last,now; gettimeofday(&last,NULL);

    while(!t.dead){
        int k=tb_key();
        if(k==24) break;                         /* ^X quit  */
        if(k==16){t.paused=!t.paused;}           /* ^P pause */

        if(!t.paused){
            /* movement */
            if((k==2004||k=='a'||k=='A')&&tb_valid(&t,t.pr,t.pc-1,t.ptype,t.prot)) t.pc--;
            if((k==2003||k=='d'||k=='D')&&tb_valid(&t,t.pr,t.pc+1,t.ptype,t.prot)) t.pc++;
            /* rotate with wall kick */
            if(k==2001||k=='w'||k=='W'||k=='z'||k=='Z'){
                int nr=(t.prot+1)%4;
                if     (tb_valid(&t,t.pr,  t.pc,  t.ptype,nr)){t.prot=nr;}
                else if(tb_valid(&t,t.pr,  t.pc+1,t.ptype,nr)){t.pc++;t.prot=nr;}
                else if(tb_valid(&t,t.pr,  t.pc-1,t.ptype,nr)){t.pc--;t.prot=nr;}
                else if(tb_valid(&t,t.pr-1,t.pc,  t.ptype,nr)){t.pr--;t.prot=nr;}
            }
            /* soft drop */
            if((k==2002||k=='s'||k=='S')&&tb_valid(&t,t.pr+1,t.pc,t.ptype,t.prot)){
                t.pr++; t.score++;
            }
            /* hard drop */
            if(k==' '){
                while(tb_valid(&t,t.pr+1,t.pc,t.ptype,t.prot)){t.pr++;t.score+=2;}
                tb_lock(&t); tb_clear(&t); tb_spawn(&t);
            }

            /* gravity */
            gettimeofday(&now,NULL);
            long us=(now.tv_sec-last.tv_sec)*1000000+(now.tv_usec-last.tv_usec);
            long tick=500000/(t.level<20?t.level:20);
            if(us>=tick){
                last=now;
                if(tb_valid(&t,t.pr+1,t.pc,t.ptype,t.prot)) t.pr++;
                else { tb_lock(&t); tb_clear(&t); tb_spawn(&t); }
            }
        }
        tb_draw(&t);
    }

    if(t.dead){
        tb_draw(&t); tb_draw_over(&t);
        struct termios raw=saved;
        raw.c_lflag&=~(ICANON|ECHO); raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
        tcsetattr(0,TCSAFLUSH,&raw);
        unsigned char d; read(0,&d,1);
    }

    tcsetattr(0,TCSAFLUSH,&saved); term_restore();
    tb_wr("\x1b[2J\x1b[H\x1b[?25h");
    return 0;
}
