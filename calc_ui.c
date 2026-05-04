/*
 * Interactive calculator UI.
 * Type the expression, Enter to evaluate, Esc/Q to quit, C to clear.
 */

#define CU_BG  "\x1b[48;5;17m"
#define CU_FG  "\x1b[38;5;51m"
#define CU_HI  "\x1b[48;5;51m\x1b[38;5;17m"
#define CU_DIM "\x1b[38;5;33m"
#define CU_YEL "\x1b[38;5;226m"
#define CU_GRN "\x1b[38;5;82m"
#define CU_RED "\x1b[38;5;196m"
#define CU_RST "\x1b[0m"

static double calc_eval(const char *s, int *err);  /* from tools.c */

static void cu_w(const char *s) { write(1, s, strlen(s)); }
static void cu_at(int r, int c) { char b[24]; snprintf(b,24,"\x1b[%d;%dH",r,c); cu_w(b); }

static void cu_paint_bg(int rows, int cols) {
    cu_w(CU_BG);
    cu_at(1,1);
    for (int r=0;r<rows;r++) {
        for (int c=0;c<cols;c++) cu_w(" ");
        if (r<rows-1) cu_w("\r\n");
    }
}

static void cu_draw(const char *expr, const char *result, int err, int rows, int cols) {
    cu_paint_bg(rows, cols);

    /* Title */
    const char *title = "  T R I U M P H    C A L C U L A T O R  ";
    int tx = (cols - (int)strlen(title)) / 2;
    cu_at(2, tx);
    cu_w(CU_FG"\x1b[1m"); cu_w(title); cu_w("\x1b[22m");

    /* Box for expression */
    int bw = 50; if (bw>cols-4) bw=cols-4;
    int bx = (cols - bw) / 2;
    int by = 6;

    /* top border */
    cu_at(by, bx); cu_w(CU_FG"\u250c");
    for (int i=0;i<bw-2;i++) cu_w("\u2500");
    cu_w("\u2510");

    /* expression line */
    cu_at(by+1, bx);    cu_w(CU_FG"\u2502 "CU_DIM"Expr: "CU_YEL);
    /* expression with cursor */
    char ebuf[256]; snprintf(ebuf,256,"%-40s", expr);
    cu_w(ebuf);
    cu_at(by+1, bx+bw-1); cu_w(CU_FG"\u2502");

    /* divider */
    cu_at(by+2, bx); cu_w(CU_FG"\u251c");
    for (int i=0;i<bw-2;i++) cu_w("\u2500");
    cu_w("\u2524");

    /* result line */
    cu_at(by+3, bx); cu_w(CU_FG"\u2502 "CU_DIM"= ");
    if (err) cu_w(CU_RED);
    else     cu_w(CU_GRN"\x1b[1m");
    char rbuf[256]; snprintf(rbuf,256,"%-44s", result);
    cu_w(rbuf);
    cu_w("\x1b[22m");
    cu_at(by+3, bx+bw-1); cu_w(CU_FG"\u2502");

    /* bottom border */
    cu_at(by+4, bx); cu_w(CU_FG"\u2514");
    for (int i=0;i<bw-2;i++) cu_w("\u2500");
    cu_w("\u2518");

    /* hints */
    cu_at(rows-2, 4);
    cu_w(CU_DIM"Type expression. Operators: + - * / % ^ ( )");
    cu_at(rows-1, 4);
    cu_w(CU_FG"ENTER "CU_DIM"evaluate   "CU_FG"C "CU_DIM"clear   "CU_FG"ESC "CU_DIM"back to menu");

    /* place visible cursor at end of expression */
    cu_at(by+1, bx + 8 + (int)strlen(expr));
    cu_w("\x1b[?25h");
    fflush(stdout);
}

static int b_calcui(Cmd *c) { (void)c;
    struct termios old, raw;
    tcgetattr(0,&old); raw=old;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    /* second-level read uses 1-deci-sec timeout to detect bare Esc */
    tcsetattr(0,TCSANOW,&raw);

    struct winsize ws; ioctl(0,TIOCGWINSZ,&ws);
    int rows = ws.ws_row?ws.ws_row:24;
    int cols = ws.ws_col?ws.ws_col:80;

    cu_w("\x1b[2J");

    char expr[200] = "";
    char result[200] = "";
    int  err = 0;

    while (1) {
        cu_draw(expr, result, err, rows, cols);
        unsigned char k;
        if (read(0,&k,1)<=0) continue;

        if (k==27) {
            /* Non-blocking peek for escape sequence bytes */
            struct termios cur; tcgetattr(0,&cur);
            struct termios tmp = cur;
            tmp.c_cc[VMIN] = 0;
            tmp.c_cc[VTIME] = 1; /* 100ms */
            tcsetattr(0,TCSANOW,&tmp);
            unsigned char seq[3]; int n=read(0,seq,3);
            tcsetattr(0,TCSANOW,&cur);
            if (n<=0) break;  /* bare Esc — exit */
            continue;          /* arrow key — ignore */
        }
        if (k=='\r' || k=='\n') {
            int e=0;
            double v = calc_eval(expr, &e);
            if (e) { strcpy(result, "error"); err=1; }
            else { snprintf(result, sizeof(result), "%.10g", v); err=0; }
            continue;
        }
        if (k==127 || k==8) {  /* backspace */
            int l=strlen(expr); if (l>0) expr[l-1]=0;
            continue;
        }
        if (k=='c' || k=='C') {
            if (expr[0]==0 && result[0]) { result[0]=0; err=0; }
            else { expr[0]=0; }
            continue;
        }
        if (k=='q' || k=='Q') break;
        /* accept printable ascii */
        if (k>=32 && k<127 && strlen(expr)<sizeof(expr)-1) {
            int l=strlen(expr); expr[l]=k; expr[l+1]=0;
        }
    }

    cu_w("\x1b[0m\x1b[2J\x1b[H");
    tcsetattr(0,TCSANOW,&old);
    return 0;
}
