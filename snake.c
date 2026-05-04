
#define SN_W      40
#define SN_H      20
#define SN_MAXLEN (SN_W * SN_H)
#define SN_TICK   120000

typedef struct { int x, y; } SnPt;

typedef struct {
    SnPt body[SN_MAXLEN];
    int  len, dx, dy, score, paused, dead;
    SnPt food;
    int  cols, rows;
} SnState;

static void sn_puts(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void sn_gotoxy(int x, int y) { char b[32]; snprintf(b,32,"\x1b[%d;%dH",y,x); sn_puts(b); }

static void sn_place_food(SnState *s) {
    for (int t = 0; t < SN_MAXLEN*2; t++) {
        int fx = 1 + rand() % SN_W;
        int fy = 1 + rand() % SN_H;
        int hit = 0;
        for (int i = 0; i < s->len; i++)
            if (s->body[i].x==fx && s->body[i].y==fy) { hit=1; break; }
        if (!hit) { s->food.x=fx; s->food.y=fy; return; }
    }
}

static void sn_draw(SnState *s) {
    int ox = (s->cols - SN_W*2) / 2; if (ox < 1) ox = 1;
    int oy = (s->rows - SN_H)   / 2; if (oy < 1) oy = 1;

    sn_gotoxy(ox, oy-1);
    printf("\x1b[1m\x1b[38;5;51m Triumph Snake \x1b[0m"
           "\x1b[38;5;245m Score:\x1b[38;5;226m\x1b[1m%-4d"
           "\x1b[0m\x1b[38;5;245m  ^X exit  ^P pause\x1b[0m", s->score);

    sn_gotoxy(ox-1, oy);
    sn_puts("\x1b[38;5;240m╔"); for(int x=0;x<SN_W;x++) sn_puts("══"); sn_puts("╗");
    for (int y=1; y<=SN_H; y++) {
        sn_gotoxy(ox-1, oy+y);
        sn_puts("║"); for(int x=0;x<SN_W;x++) sn_puts("  "); sn_puts("║");
    }
    sn_gotoxy(ox-1, oy+SN_H+1);
    sn_puts("╚"); for(int x=0;x<SN_W;x++) sn_puts("══"); sn_puts("╝\x1b[0m");

    sn_gotoxy(ox+(s->food.x-1)*2, oy+s->food.y);
    sn_puts("\x1b[38;5;196m\x1b[1m●\x1b[0m");

    for (int i=0; i<s->len; i++) {
        sn_gotoxy(ox+(s->body[i].x-1)*2, oy+s->body[i].y);
        sn_puts(i==0 ? "\x1b[38;5;82m\x1b[1m■\x1b[0m" : "\x1b[38;5;40m■\x1b[0m");
    }

    if (s->paused) {
        sn_gotoxy(ox+SN_W-3, oy+SN_H/2);
        sn_puts("\x1b[1m\x1b[38;5;226m  PAUSED  \x1b[0m");
    }
    fflush(stdout);
}

static void sn_draw_dead(SnState *s) {
    int ox = (s->cols - SN_W*2) / 2; if (ox < 1) ox = 1;
    int oy = (s->rows - SN_H)   / 2; if (oy < 1) oy = 1;
    sn_gotoxy(ox+SN_W-6, oy+SN_H/2-1);
    sn_puts("\x1b[1m\x1b[38;5;196m  GAME OVER  \x1b[0m");
    sn_gotoxy(ox+SN_W-6, oy+SN_H/2+1);
    printf("\x1b[38;5;245m  Score: \x1b[38;5;226m\x1b[1m%d\x1b[0m", s->score);
    sn_gotoxy(ox+SN_W-6, oy+SN_H/2+2);
    sn_puts("\x1b[38;5;245m  Press any key...\x1b[0m");
    fflush(stdout);
}

static int sn_read_key(void) {

    fd_set fds; struct timeval tv = {0,0};
    FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) <= 0) return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return -1;
    if (c == 27) {
        unsigned char s[2];
        tv.tv_usec = 50000;
        FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO+1,&fds,NULL,NULL,&tv)<=0) return 27;
        if (read(STDIN_FILENO,&s[0],1)<=0) return 27;
        if (s[0]!='[') return 27;
        if (read(STDIN_FILENO,&s[1],1)<=0) return 27;
        switch(s[1]) {
            case 'A': return 1001;
            case 'B': return 1002;
            case 'C': return 1003;
            case 'D': return 1004;
        }
        return 27;
    }
    return (int)c;
}

static int b_snake(Cmd *cmd) {
    (void)cmd;

    struct termios saved;
    term_raw();
    tcgetattr(STDIN_FILENO, &saved);

    struct winsize ws;
    int tcols = 80, trows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)==0 && ws.ws_col && ws.ws_row) {
        tcols = ws.ws_col; trows = ws.ws_row;
    }

    srand((unsigned)time(NULL));
    sn_puts("\x1b[2J\x1b[H\x1b[?25l");

    SnState s;
    memset(&s, 0, sizeof(s));
    s.cols = tcols; s.rows = trows;
    s.len  = 3;
    s.dx   = 1; s.dy = 0;

    for (int i = 0; i < s.len; i++) {
        s.body[i].x = SN_W/2 - i;
        s.body[i].y = SN_H/2;
    }
    sn_place_food(&s);

    while (!s.dead) {
        int key = sn_read_key();

        if (key == 24)  break;
        if (key == 16)  { s.paused = !s.paused; }

        if (!s.paused) {

            if      ((key==1001||key=='w'||key=='W') && s.dy!= 1) { s.dx=0;  s.dy=-1; }
            else if ((key==1002||key=='s'||key=='S') && s.dy!=-1) { s.dx=0;  s.dy= 1; }
            else if ((key==1003||key=='d'||key=='D') && s.dx!=-1) { s.dx=1;  s.dy= 0; }
            else if ((key==1004||key=='a'||key=='A') && s.dx!= 1) { s.dx=-1; s.dy= 0; }

            SnPt head = { s.body[0].x + s.dx, s.body[0].y + s.dy };

            if (head.x<1||head.x>SN_W||head.y<1||head.y>SN_H) { s.dead=1; pc_play(SND_GAMEOVER); break; }

            for (int i=1; i<s.len; i++)
                if (s.body[i].x==head.x && s.body[i].y==head.y) { s.dead=1; pc_play(SND_GAMEOVER); break; }
            if (s.dead) break;

            int ate = (head.x==s.food.x && head.y==s.food.y);

            if (!ate) s.len--;
            memmove(&s.body[1], &s.body[0], s.len * sizeof(SnPt));
            s.body[0] = head;
            s.len++;

            if (ate) { s.score += 10; sn_place_food(&s); pc_play(SND_NOM); }

            sn_draw(&s);
            usleep(SN_TICK);
        } else {
            sn_draw(&s);
            usleep(50000);
        }
    }

    if (s.dead) {
        sn_draw(&s);
        sn_draw_dead(&s);

        struct termios raw;
        tcgetattr(STDIN_FILENO, &raw);
        raw.c_lflag &= ~(ICANON|ECHO);
        raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        unsigned char dummy;
        read(STDIN_FILENO, &dummy, 1);
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    term_restore();
    sn_puts("\x1b[2J\x1b[H\x1b[?25h");
    return 0;
}
