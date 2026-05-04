
#define CK_W      70
#define CK_H      10
#define CK_GROUND (CK_H - 1)
#define CK_TICK   80000

typedef struct {
    int cols, rows;
    float y;
    float vy;
    int jumping;
    int score;
    int highscore;
    int dead;
    int paused;
    int obstacles[8];  /* x positions */
    int obs_type[8];   /* 0=cactus, 1=bird */
    int frame;
    float speed;
} CkState;

static void ck_puts(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void ck_gotoxy(int x, int y) { char b[32]; snprintf(b,32,"\x1b[%d;%dH",y,x); ck_puts(b); }

static void ck_init(CkState *s) {
    memset(s, 0, sizeof(*s));
    s->y = CK_GROUND;
    s->speed = 1.0f;
    for (int i=0; i<8; i++) s->obstacles[i] = -1;
    s->obstacles[0] = CK_W + 15;
    s->obs_type[0] = 0;
}

/* 3-frame chicken sprite */
static const char *CHICKEN_RUN[2][3] = {
    { "\x1b[38;5;220m▄\x1b[38;5;231m◢\x1b[38;5;196m▼\x1b[0m",     /* body + comb */
      "\x1b[38;5;220m▀\x1b[38;5;220m▙\x1b[38;5;226m◤\x1b[0m",
      "\x1b[38;5;208m╱\x1b[0m \x1b[38;5;208m╲\x1b[0m" },
    { "\x1b[38;5;220m▄\x1b[38;5;231m◢\x1b[38;5;196m▼\x1b[0m",
      "\x1b[38;5;220m▀\x1b[38;5;220m▙\x1b[38;5;226m◤\x1b[0m",
      "\x1b[38;5;208m╲\x1b[0m \x1b[38;5;208m╱\x1b[0m" },
};

static const char *CHICKEN_JUMP[3] = {
    "\x1b[38;5;220m▄\x1b[38;5;231m◢\x1b[38;5;196m▼\x1b[0m",
    "\x1b[38;5;226m╱\x1b[38;5;220m▙\x1b[38;5;226m╲\x1b[0m",
    "\x1b[38;5;208m┘\x1b[0m \x1b[38;5;208m└\x1b[0m",
};

static void ck_draw_chicken(CkState *s, int ox, int oy) {
    int cx = ox + 4;
    int cy = oy + (int)s->y;
    const char **sprite;
    if (s->jumping) sprite = CHICKEN_JUMP;
    else sprite = CHICKEN_RUN[(s->frame/4) % 2];

    /* 3 rows tall */
    ck_gotoxy(cx, cy-2); ck_puts(sprite[0]);
    ck_gotoxy(cx, cy-1); ck_puts(sprite[1]);
    ck_gotoxy(cx, cy);   ck_puts(sprite[2]);
}

static void ck_draw_cactus(int x, int y) {
    ck_gotoxy(x, y-2); ck_puts("\x1b[38;5;34m▲\x1b[0m");
    ck_gotoxy(x, y-1); ck_puts("\x1b[38;5;34m█\x1b[0m");
    ck_gotoxy(x, y);   ck_puts("\x1b[38;5;34m█\x1b[0m");
}

static void ck_draw_bird(int x, int y, int frame) {
    ck_gotoxy(x, y-3);
    if ((frame/3)&1) ck_puts("\x1b[38;5;51m▀▀\x1b[0m");
    else             ck_puts("\x1b[38;5;51m▄▄\x1b[0m");
}

static void ck_draw(CkState *s) {
    int ox = (s->cols - CK_W) / 2; if (ox<1) ox=1;
    int oy = (s->rows - CK_H - 4) / 2 + 2; if (oy<2) oy=2;

    ck_puts("\x1b[2J");

    /* header */
    ck_gotoxy(ox, oy-2);
    printf("\x1b[1m\x1b[38;5;51m Triumph Chicken \x1b[0m"
           "\x1b[38;5;245m  Score: \x1b[1m\x1b[38;5;226m%-5d \x1b[0m"
           "\x1b[38;5;245m Hi: \x1b[38;5;208m\x1b[1m%-5d\x1b[0m"
           "\x1b[38;5;245m  SPACE jump  ^P pause  ^X exit\x1b[0m",
           s->score, s->highscore);

    /* sky (dots for stars, clouds) */
    for (int y=0; y<CK_H-1; y++) {
        ck_gotoxy(ox, oy+y);
        for (int x=0; x<CK_W; x++) {
            int seed = (x + s->score/5) % 40;
            if (y==1 && seed==3) ck_puts("\x1b[38;5;244m·\x1b[0m");
            else if (y==2 && seed==17) ck_puts("\x1b[38;5;244m.\x1b[0m");
            else if (y==0 && seed==29) ck_puts("\x1b[38;5;250m~\x1b[0m");
            else ck_puts(" ");
        }
    }

    /* ground */
    ck_gotoxy(ox, oy+CK_H);
    for (int x=0; x<CK_W; x++) {
        int p = (x + s->score/2) % 6;
        if (p==0)      ck_puts("\x1b[38;5;94m═\x1b[0m");
        else if (p==3) ck_puts("\x1b[38;5;130m═\x1b[0m");
        else           ck_puts("\x1b[38;5;94m═\x1b[0m");
    }

    /* obstacles */
    for (int i=0; i<8; i++) {
        if (s->obstacles[i] < 0) continue;
        int x = ox + s->obstacles[i];
        if (x < ox || x > ox + CK_W) continue;
        if (s->obs_type[i] == 0) ck_draw_cactus(x, oy+CK_GROUND);
        else ck_draw_bird(x, oy+CK_GROUND, s->frame);
    }

    /* chicken - pass oy so function computes cy = oy + s->y */
    ck_draw_chicken(s, ox, oy);

    if (s->paused) {
        ck_gotoxy(ox+CK_W/2-4, oy+CK_H/2);
        ck_puts("\x1b[48;5;226m\x1b[38;5;16m\x1b[1m PAUSED \x1b[0m");
    }
    if (s->dead) {
        ck_gotoxy(ox+CK_W/2-8, oy+CK_H/2-1);
        ck_puts("\x1b[48;5;196m\x1b[38;5;231m\x1b[1m   GAME OVER   \x1b[0m");
        ck_gotoxy(ox+CK_W/2-10, oy+CK_H/2+1);
        ck_puts("\x1b[38;5;245mR to retry, ^X exit\x1b[0m");
    }
    fflush(stdout);
}

static int ck_collide(CkState *s) {
    int cx = 4;  /* chicken x relative to play area */
    int cy = (int)s->y;
    for (int i=0; i<8; i++) {
        if (s->obstacles[i] < 0) continue;
        int ox_pos = s->obstacles[i];
        if (ox_pos < cx-1 || ox_pos > cx+1) continue;
        if (s->obs_type[i] == 0) {
            /* cactus on ground - only hits if on ground */
            if (cy >= CK_GROUND - 1) return 1;
        } else {
            /* bird flies high - only hits if jumping */
            if (cy <= CK_GROUND - 2) return 1;
        }
    }
    return 0;
}

static void ck_tick(CkState *s) {
    if (s->paused || s->dead) return;
    s->frame++;

    /* physics - gravity */
    if (s->jumping) {
        s->y += s->vy;
        s->vy += 0.25f;
        if (s->y >= CK_GROUND) {
            s->y = CK_GROUND;
            s->vy = 0;
            s->jumping = 0;
        }
    }

    /* move obstacles */
    for (int i=0; i<8; i++) {
        if (s->obstacles[i] < 0) continue;
        s->obstacles[i] -= (int)s->speed;
        if (s->obstacles[i] < -3) {
            s->obstacles[i] = -1;
            s->score++;
        }
    }

    /* spawn new obstacle */
    int farthest = 0;
    for (int i=0; i<8; i++) {
        if (s->obstacles[i] > farthest) farthest = s->obstacles[i];
    }
    if (farthest < CK_W - 25 && (rand() % 40) == 0) {
        for (int i=0; i<8; i++) {
            if (s->obstacles[i] < 0) {
                s->obstacles[i] = CK_W + 2 + (rand() % 15);
                s->obs_type[i] = (s->score > 10 && rand() % 4 == 0) ? 1 : 0;
                break;
            }
        }
    }

    /* speed up */
    s->speed = 1.0f + (float)s->score * 0.02f;
    if (s->speed > 3.0f) s->speed = 3.0f;

    /* collision */
    if (ck_collide(s)) {
        s->dead = 1;
        if (s->score > s->highscore) s->highscore = s->score;
        pc_play(SND_GAMEOVER);
    }
}

static int ck_readkey(void) {
    unsigned char c; int r = read(STDIN_FILENO, &c, 1);
    if (r<=0) return 0;
    if (c==0x1b) {
        unsigned char seq[3]; int n=read(STDIN_FILENO,seq,3);
        if (n>=2 && seq[0]=='[' && seq[1]=='A') return ' ';
        return 27;
    }
    return c;
}

static int b_chicken(Cmd *c) { (void)c;
    struct termios old, raw;
    tcgetattr(0,&old); raw=old;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=0;
    tcsetattr(0,TCSANOW,&raw);

    struct winsize ws; ioctl(0,TIOCGWINSZ,&ws);
    int cols=ws.ws_col?ws.ws_col:80, rows=ws.ws_row?ws.ws_row:24;

    ck_puts("\x1b[2J\x1b[?25l");

    CkState s; ck_init(&s);
    s.cols=cols; s.rows=rows;
    srand(time(NULL));

    int hi = 0;

    while (1) {
        s.highscore = hi;
        ck_draw(&s);

        int k = ck_readkey();
        if (k) {
            if (k==' ' || k=='w' || k=='W') {
                if (!s.jumping && !s.dead) {
                    s.jumping = 1;
                    s.vy = -2.0f;
                    pc_play(SND_JUMP);
                }
                if (s.dead) goto quit;
            }
            else if (k==16) s.paused = !s.paused;
            else if (k=='r' || k=='R') {
                if (s.dead) {
                    hi = s.highscore;
                    ck_init(&s);
                    s.cols=cols; s.rows=rows;
                    s.highscore = hi;
                }
            }
            else if (k==24 || k==27 || k=='q') goto quit;
        }

        ck_tick(&s);
        if (s.score > hi) hi = s.score;

        usleep(CK_TICK);
    }

quit:
    ck_puts("\x1b[?25h\x1b[2J\x1b[H");
    tcsetattr(0,TCSANOW,&old);
    return 0;
}
