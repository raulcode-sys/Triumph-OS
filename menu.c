/*
 * Triumph fullscreen menu launcher.
 * Three-column layout: help bar left, menu center, fetch right.
 * Arrow keys + Enter to select. Loops back here after every program exits.
 */

#define MN_BG  "\x1b[48;5;17m"
#define MN_FG  "\x1b[38;5;51m"
#define MN_HI  "\x1b[48;5;51m\x1b[38;5;17m"
#define MN_DIM "\x1b[38;5;33m"
#define MN_DIM2 "\x1b[38;5;67m"
#define MN_YEL "\x1b[38;5;226m"
#define MN_GRN "\x1b[38;5;82m"
#define MN_RED "\x1b[38;5;196m"

static int b_calcui(Cmd *c);  /* forward */

static const struct { const char *label; const char *desc; const char *cmd; int kind; } MNU[] = {
    {"Snake",      "Classic snake game",            "snake",   0},
    {"Tetris",     "Falling block puzzle",          "tetris",  0},
    {"Pongy",      "Pong with AI opponent",         "pongy",   0},
    {"Chicken",    "Chrome-dino runner",            "chicken", 0},
    {"Calculator", "Interactive calculator",        NULL,      1},
    {"Editor",     "Text editor (nano-style)",      "edit",    0},
    {"Files",      "File explorer",                 "files",   0},
    {"Web",        "HTTP browser (ethernet)",       "web",     0},
    {"Reboot",     "Restart the machine",           "reboot",  0},
    {"Poweroff",   "Shut down",                     "poweroff",0},
};
#define MNU_N ((int)(sizeof(MNU)/sizeof(MNU[0])))

static const char *MN_HELP_LINES[] = {
    "── HELP ──",
    "",
    "↑↓ / W S",
    "  move",
    "ENTER",
    "  select",
    "ESC / Q",
    "  exit",
    "",
    "── KEYS ──",
    "",
    "Snake",
    "  WASD move",
    "  ^X quit",
    "",
    "Tetris",
    "  ←→ move",
    "  ↑ rotate",
    "  Space drop",
    "",
    "Pongy",
    "  W S move",
    "  ^X quit",
    "",
    "Chicken",
    "  Space jump",
    "  R retry",
    "",
    "Calc",
    "  type expr",
    "  Enter eval",
    "  C clear",
    NULL
};

static void mn_w(const char *s) { write(1, s, strlen(s)); }
static void mn_at(int r, int c) { char b[24]; snprintf(b,24,"\x1b[%d;%dH",r,c); mn_w(b); }

/* Returns -2 on timeout, 0 on no key, otherwise the key. */
static int mn_readkey_timeout(int seconds) {
    fd_set fds;
    FD_ZERO(&fds); FD_SET(0, &fds);
    struct timeval tv = { seconds, 0 };
    int r = select(1, &fds, NULL, NULL, &tv);
    if (r <= 0) return -2;  /* timeout */
    unsigned char c;
    if (read(0,&c,1)<=0) return 0;
    if (c==0x1b) {
        /* peek for sequence with short timeout */
        struct timeval tv2 = { 0, 100000 };
        FD_ZERO(&fds); FD_SET(0,&fds);
        if (select(1,&fds,NULL,NULL,&tv2) <= 0) return 27;
        unsigned char seq[3]; int n=read(0,seq,3);
        if (n>=2 && seq[0]=='[') {
            if (seq[1]=='A') return 'U';
            if (seq[1]=='B') return 'D';
            if (seq[1]=='C') return 'R';
            if (seq[1]=='D') return 'L';
        }
        return 27;
    }
    return c;
}

static void mn_paint_bg(int rows, int cols) {
    mn_w(MN_BG);
    mn_at(1,1);
    for (int r=0;r<rows;r++) {
        for (int c=0;c<cols;c++) mn_w(" ");
        if (r<rows-1) mn_w("\r\n");
    }
}

static const char *MN_BANNER[] = {
    "  ████████╗██████╗ ██╗██╗   ██╗███╗   ███╗██████╗ ██╗  ██╗",
    "  ╚══██╔══╝██╔══██╗██║██║   ██║████╗ ████║██╔══██╗██║  ██║",
    "     ██║   ██████╔╝██║██║   ██║██╔████╔██║██████╔╝███████║",
    "     ██║   ██╔══██╗██║██║   ██║██║╚██╔╝██║██╔═══╝ ██╔══██║",
    "     ██║   ██║  ██║██║╚██████╔╝██║ ╚═╝ ██║██║     ██║  ██║",
    "     ╚═╝   ╚═╝  ╚═╝╚═╝ ╚═════╝ ╚═╝     ╚═╝╚═╝     ╚═╝  ╚═╝",
    NULL
};

static void mn_fetch_lines(char out[][64], int *n_out) {
    int n = 0;
    struct utsname u; uname(&u);
    char host[64]=""; gethostname(host, sizeof(host));
    const char *user = getenv("USER"); if (!user) user = "root";

    /* T logo - fits in ~22 char column */
    snprintf(out[n++], 64, "TTTTTTTTTTTTTTTTTTTTT");
    snprintf(out[n++], 64, "T:::::::::::::::::::T");
    snprintf(out[n++], 64, "T:::::TT:::::TT:::::T");
    snprintf(out[n++], 64, "TTTTT  T:::::T  TTTTT");
    snprintf(out[n++], 64, "       T:::::T       ");
    snprintf(out[n++], 64, "       T:::::T       ");
    snprintf(out[n++], 64, "       T:::::T       ");
    snprintf(out[n++], 64, "     TT:::::::TT     ");
    snprintf(out[n++], 64, "     T:::::::::T     ");
    snprintf(out[n++], 64, "     TTTTTTTTTTT     ");
    out[n++][0]=0;
    snprintf(out[n++], 64, "── INFO ──");
    out[n++][0]=0;
    snprintf(out[n++], 64, "user: %s", user);
    snprintf(out[n++], 64, "host: %s", host);
    snprintf(out[n++], 64, "os:   Triumph");
    snprintf(out[n++], 64, "ver:  1.0.0");
    snprintf(out[n++], 64, "arch: %s", u.machine);
    out[n++][0]=0;

    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        long up = si.uptime;
        snprintf(out[n++], 64, "up:   %ldh %ldm", up/3600, (up/60)%60);
        unsigned long mt = si.totalram * si.mem_unit / 1024 / 1024;
        unsigned long mu = (si.totalram - si.freeram) * si.mem_unit / 1024 / 1024;
        snprintf(out[n++], 64, "mem:  %lu/%luM", mu, mt);
        snprintf(out[n++], 64, "load: %.2f", si.loads[0] / 65536.0);
    }
    out[n++][0]=0;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(out[n++], 64, "time: %02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    snprintf(out[n++], 64, "date: %04d-%02d-%02d", tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday);
    out[n++][0]=0;
    snprintf(out[n++], 64, "── COLOURS ──");
    out[n++][0]=0;
    snprintf(out[n++], 64, "clr -r/-o/-y");
    snprintf(out[n++], 64, "clr -g/-b/-m");
    snprintf(out[n++], 64, "clr -c/-p/-w");
    snprintf(out[n++], 64, "clr -k reset");

    *n_out = n;
}

static void mn_draw(int sel, int rows, int cols) {
    mn_paint_bg(rows, cols);

    int by = 2;
    int banner_w = 58;
    int bx = (cols - banner_w) / 2; if (bx<1) bx=1;
    for (int i=0; MN_BANNER[i]; i++) {
        mn_at(by+i, bx);
        mn_w(MN_FG); mn_w(MN_BANNER[i]);
    }

    const char *tag = "« Triumph OS »";
    int tagx = (cols - (int)strlen(tag)) / 2;
    mn_at(by+7, tagx<1?1:tagx);
    mn_w(MN_YEL); mn_w(tag);

    mn_at(by+8, 1);
    mn_w(MN_DIM);
    for (int c=0;c<cols;c++) mn_w("═");

    int col_top = by + 10;
    int col_h   = rows - col_top - 2;

    int left_w  = 18;
    int right_w = 22;
    int center_x = left_w + 2;
    int center_w = cols - left_w - right_w - 4;
    if (center_w < 30) center_w = 30;
    int right_x = cols - right_w - 1;

    /* Left col: help */
    for (int i=0; MN_HELP_LINES[i] && i<col_h; i++) {
        mn_at(col_top + i, 2);
        const char *line = MN_HELP_LINES[i];
        if (line[0]=='-' && line[1]=='-') {
            mn_w(MN_YEL); mn_w(line);
        } else if (line[0]==' ') {
            mn_w(MN_DIM2); mn_w(line);
        } else {
            mn_w(MN_FG); mn_w(line);
        }
    }

    /* Right col: fetch with logo on top */
    char info[36][64];
    int n_info = 0;
    mn_fetch_lines(info, &n_info);
    for (int i=0; i<n_info && i<col_h; i++) {
        mn_at(col_top + i, right_x);
        const char *line = info[i];
        if (line[0]=='-' && line[1]=='-') {
            mn_w(MN_YEL); mn_w(line);
        } else if (line[0]==0) {
            /* blank */
        } else if (line[0]=='T' || (line[0]==' ' && (line[7]=='T' || line[5]=='T'))) {
            /* logo line — print in bright cyan, bold */
            mn_w(MN_FG"\x1b[1m"); mn_w(line); mn_w("\x1b[22m");
        } else {
            mn_w(MN_FG);
            char head[8]={0};
            int k=0;
            while (line[k] && line[k]!=':' && k<5) { head[k]=line[k]; k++; }
            head[k]=0;
            mn_w(head);
            if (line[k]==':') {
                mn_w(MN_DIM2);
                mn_w(line+k);
            }
        }
    }

    /* Vertical separators */
    for (int y=col_top-1; y<col_top+col_h+1; y++) {
        mn_at(y, left_w + 1);
        mn_w(MN_DIM); mn_w("│");
        mn_at(y, right_x - 1);
        mn_w("│");
    }

    /* Centre col: menu */
    int item_w = center_w - 2; if (item_w>50) item_w=50;
    int my = col_top + (col_h - MNU_N) / 2;
    if (my < col_top + 2) my = col_top + 2;
    int mx = center_x + (center_w - item_w) / 2;
    if (mx < center_x) mx = center_x;

    mn_at(my-2, mx);
    mn_w(MN_YEL"\x1b[1m"); mn_w("── MENU ──"); mn_w("\x1b[22m");

    for (int i=0; i<MNU_N; i++) {
        mn_at(my+i, mx);
        if (i == sel) {
            mn_w(MN_HI"\x1b[1m");
            char line[64];
            snprintf(line,64,"  ▶ %-12s  %-22s", MNU[i].label, MNU[i].desc);
            mn_w(line);
            mn_w("\x1b[22m"); mn_w(MN_BG MN_FG);
        } else {
            mn_w(MN_FG);
            char line[64];
            snprintf(line,64,"    %-12s  ", MNU[i].label);
            mn_w(line);
            mn_w(MN_DIM);
            mn_w(MNU[i].desc);
        }
    }

    /* Footer */
    mn_at(rows-1, 1);
    mn_w(MN_DIM);
    for (int c=0;c<cols;c++) mn_w("═");
    mn_at(rows, 4);
    mn_w(MN_FG"↑↓ "MN_DIM"move  "MN_FG"ENTER "MN_DIM"select  "MN_FG"ESC "MN_DIM"hide menu");

    fflush(stdout);
}

static int b_menu(Cmd *c) { (void)c;
    struct termios old, raw;
    tcgetattr(0,&old); raw=old;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    tcsetattr(0,TCSANOW,&raw);

    struct winsize ws; ioctl(0,TIOCGWINSZ,&ws);
    int rows = ws.ws_row?ws.ws_row:24;
    int cols = ws.ws_col?ws.ws_col:80;

    mn_w("\x1b[?25l\x1b[2J");

    int sel = 0;

    while (1) {
        mn_draw(sel, rows, cols);
        int k = mn_readkey_timeout(1);
        if (k == -2) continue;  /* timeout — just redraw */
        if      (k=='U' || k=='k' || k=='w' || k=='W') sel = (sel - 1 + MNU_N) % MNU_N;
        else if (k=='D' || k=='j' || k=='s' || k=='S') sel = (sel + 1) % MNU_N;
        else if (k==13 || k==10) {
            mn_w("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
            tcsetattr(0,TCSANOW,&old);

            if (MNU[sel].kind == 1) {
                Cmd dc = {0};
                b_calcui(&dc);
            } else if (MNU[sel].cmd) {
                char buf[256];
                strncpy(buf, MNU[sel].cmd, sizeof(buf)-1);
                buf[sizeof(buf)-1]=0;
                run_line(buf);
            }

            tcsetattr(0,TCSANOW,&raw);
            mn_w("\x1b[?25l\x1b[2J");
        }
        else if (k==27 || k=='q' || k=='Q') break;
    }

    mn_w("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    tcsetattr(0,TCSANOW,&old);
    return 0;
}
