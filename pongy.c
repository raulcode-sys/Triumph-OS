
#define PG_W      60
#define PG_H      20
#define PG_PADDLE 4
#define PG_TICK   50000

typedef struct {
    int cols, rows;
    float bx, by;
    float vx, vy;
    int p1y, p2y;
    int s1, s2;
    int paused, dead;
    int winner;
} PgState;

static void pg_puts(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void pg_gotoxy(int x, int y) { char b[32]; snprintf(b,32,"\x1b[%d;%dH",y,x); pg_puts(b); }

static void pg_reset_ball(PgState *s, int dir) {
    s->bx = PG_W / 2.0f;
    s->by = PG_H / 2.0f;
    s->vx = dir * 0.6f;
    s->vy = ((rand() % 100) / 100.0f - 0.5f) * 0.4f;
}

static void pg_init(PgState *s) {
    memset(s, 0, sizeof(*s));
    s->p1y = PG_H/2 - PG_PADDLE/2;
    s->p2y = PG_H/2 - PG_PADDLE/2;
    pg_reset_ball(s, (rand()&1) ? 1 : -1);
}

static void pg_draw(PgState *s) {
    int ox = (s->cols - PG_W) / 2; if (ox<1) ox=1;
    int oy = (s->rows - PG_H) / 2; if (oy<1) oy=1;

    pg_gotoxy(ox, oy-1);
    printf("\x1b[1m\x1b[38;5;51m Triumph Pongy \x1b[0m"
           "\x1b[38;5;245m  P1:\x1b[1m\x1b[38;5;226m%d\x1b[0m"
           "\x1b[38;5;245m  P2:\x1b[1m\x1b[38;5;226m%d\x1b[0m"
           "\x1b[38;5;245m  W/S move  ^P pause  ^X exit\x1b[0m", s->s1, s->s2);

    /* frame */
    pg_gotoxy(ox-1, oy);
    pg_puts("\x1b[38;5;240m╔"); for(int x=0;x<PG_W;x++) pg_puts("═"); pg_puts("╗");
    for (int y=1; y<=PG_H; y++) {
        pg_gotoxy(ox-1, oy+y);
        pg_puts("║"); for(int x=0;x<PG_W;x++) pg_puts(" "); pg_puts("║");
    }
    pg_gotoxy(ox-1, oy+PG_H+1);
    pg_puts("╚"); for(int x=0;x<PG_W;x++) pg_puts("═"); pg_puts("╝\x1b[0m");

    /* centre line */
    for (int y=1; y<=PG_H; y+=2) {
        pg_gotoxy(ox+PG_W/2, oy+y);
        pg_puts("\x1b[38;5;240m│\x1b[0m");
    }

    /* paddles */
    for (int i=0; i<PG_PADDLE; i++) {
        pg_gotoxy(ox+1, oy+s->p1y+i);
        pg_puts("\x1b[38;5;51m\x1b[1m█\x1b[0m");
        pg_gotoxy(ox+PG_W-2, oy+s->p2y+i);
        pg_puts("\x1b[38;5;196m\x1b[1m█\x1b[0m");
    }

    /* ball */
    int bx = (int)s->bx, by = (int)s->by;
    if (bx>=1 && bx<=PG_W && by>=1 && by<=PG_H) {
        pg_gotoxy(ox+bx, oy+by);
        pg_puts("\x1b[38;5;226m\x1b[1m●\x1b[0m");
    }

    if (s->paused) {
        pg_gotoxy(ox+PG_W/2-4, oy+PG_H/2);
        pg_puts("\x1b[48;5;226m\x1b[38;5;16m\x1b[1m PAUSED \x1b[0m");
    }
    if (s->winner) {
        pg_gotoxy(ox+PG_W/2-8, oy+PG_H/2);
        printf("\x1b[48;5;196m\x1b[38;5;231m\x1b[1m  P%d WINS!  \x1b[0m", s->winner);
        pg_gotoxy(ox+PG_W/2-10, oy+PG_H/2+1);
        pg_puts("\x1b[38;5;245mPress any key\x1b[0m");
    }
    fflush(stdout);
}

static void pg_tick(PgState *s) {
    if (s->paused || s->winner) return;

    s->bx += s->vx;
    s->by += s->vy;

    /* top/bottom bounce */
    if (s->by < 1)    { s->by = 1;    s->vy = -s->vy; pc_tone(440, 15); }
    if (s->by > PG_H) { s->by = PG_H; s->vy = -s->vy; pc_tone(440, 15); }

    /* paddle collision — left */
    if (s->bx <= 2 && s->vx < 0) {
        int py = (int)s->by;
        if (py >= s->p1y && py < s->p1y + PG_PADDLE) {
            s->vx = -s->vx * 1.05f;
            s->vy += ((py - s->p1y) - PG_PADDLE/2) * 0.15f;
            s->bx = 2;
            pc_play(SND_BLIP);
        }
    }
    /* paddle collision — right */
    if (s->bx >= PG_W-1 && s->vx > 0) {
        int py = (int)s->by;
        if (py >= s->p2y && py < s->p2y + PG_PADDLE) {
            s->vx = -s->vx * 1.05f;
            s->vy += ((py - s->p2y) - PG_PADDLE/2) * 0.15f;
            s->bx = PG_W - 1;
            pc_play(SND_BLIP_LO);
        }
    }

    /* scoring */
    if (s->bx < 0) {
        s->s2++;
        pc_play(SND_SCORE);
        if (s->s2 >= 5) { s->winner = 2; pc_play(SND_GAMEOVER); }
        else pg_reset_ball(s, 1);
    }
    if (s->bx > PG_W+1) {
        s->s1++;
        pc_play(SND_SCORE);
        if (s->s1 >= 5) { s->winner = 1; pc_play(SND_GAMEOVER); }
        else pg_reset_ball(s, -1);
    }

    /* AI for player 2 */
    int target = (int)s->by - PG_PADDLE/2;
    if (s->p2y < target) s->p2y++;
    else if (s->p2y > target) s->p2y--;
    if (s->p2y < 1) s->p2y = 1;
    if (s->p2y > PG_H - PG_PADDLE) s->p2y = PG_H - PG_PADDLE;
}

static int pg_readkey(void) {
    unsigned char c; int r = read(STDIN_FILENO, &c, 1);
    if (r<=0) return 0;
    if (c==0x1b) {
        unsigned char seq[3]; int n=read(STDIN_FILENO,seq,3);
        if (n>=2 && seq[0]=='[') {
            if (seq[1]=='A') return 'W';
            if (seq[1]=='B') return 'S';
        }
        return 27;
    }
    return c;
}

static int b_pongy(Cmd *c) { (void)c;
    struct termios old, raw;
    tcgetattr(0,&old); raw=old;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=0;
    tcsetattr(0,TCSANOW,&raw);

    struct winsize ws; ioctl(0,TIOCGWINSZ,&ws);
    int cols=ws.ws_col?ws.ws_col:80, rows=ws.ws_row?ws.ws_row:24;

    pg_puts("\x1b[2J\x1b[?25l");

    PgState s; pg_init(&s);
    s.cols=cols; s.rows=rows;
    srand(time(NULL));

    long acc=0;
    while (!s.dead) {
        pg_puts("\x1b[2J");
        pg_draw(&s);

        int k = pg_readkey();
        if (k) {
            if (k=='w' || k=='W') { if (s.p1y>1) s.p1y--; }
            else if (k=='s' || k=='S') { if (s.p1y<PG_H-PG_PADDLE) s.p1y++; }
            else if (k==16) s.paused=!s.paused;
            else if (k==24 || k==27 || k=='q') s.dead=1;
            else if (s.winner) s.dead=1;
        }

        pg_tick(&s);

        usleep(PG_TICK);
        acc+=PG_TICK;
    }

    pg_puts("\x1b[?25h\x1b[2J\x1b[H");
    tcsetattr(0,TCSANOW,&old);
    return 0;
}
