
typedef struct {
    const char *s;
    int         pos;
} CalcParser;

static void  calc_skip(CalcParser *p) { while (p->s[p->pos]==' '||p->s[p->pos]=='\t') p->pos++; }
static double calc_expr(CalcParser *p);

static double calc_num(CalcParser *p) {
    calc_skip(p);
    if (p->s[p->pos] == '(') {
        p->pos++;
        double v = calc_expr(p);
        calc_skip(p);
        if (p->s[p->pos] == ')') p->pos++;
        return v;
    }

    if (p->s[p->pos] == '-') { p->pos++; return -calc_num(p); }
    if (p->s[p->pos] == '+') { p->pos++; return  calc_num(p); }

    char *end;
    double v = strtod(p->s + p->pos, &end);
    p->pos = (int)(end - p->s);
    return v;
}

static double calc_pow(CalcParser *p) {
    double base = calc_num(p);
    calc_skip(p);
    if (p->s[p->pos] == '^') {
        p->pos++;
        double exp = calc_pow(p);
        return pow(base, exp);
    }
    return base;
}

static double calc_term(CalcParser *p) {
    double v = calc_pow(p);
    while (1) {
        calc_skip(p);
        char op = p->s[p->pos];
        if (op != '*' && op != '/' && op != '%') break;
        p->pos++;
        double r = calc_pow(p);
        if      (op == '*') v *= r;
        else if (op == '/') v = (r != 0) ? v/r : 0;
        else                v = fmod(v, r);
    }
    return v;
}

static double calc_expr(CalcParser *p) {
    double v = calc_term(p);
    while (1) {
        calc_skip(p);
        char op = p->s[p->pos];
        if (op != '+' && op != '-') break;
        p->pos++;
        double r = calc_term(p);
        v = (op == '+') ? v+r : v-r;
    }
    return v;
}

static double calc_eval(const char *s, int *err) {
    if (err) *err = 0;
    if (!s || !*s) { if (err) *err = 1; return 0; }
    CalcParser p; p.s = s; p.pos = 0;
    double v = calc_expr(&p);
    calc_skip(&p);
    if (s[p.pos] != 0) { if (err) *err = 1; }
    return v;
}

static int b_calc(Cmd *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "usage: calc <expression>\n");
        fprintf(stderr, "  e.g. calc 2+2  calc \"3*(4+5)\"  calc 2^10  calc 10%%3\n");
        return 1;
    }

    char expr[512] = "";
    for (int i = 1; i < cmd->argc; i++) {
        if (i > 1) strncat(expr, " ", sizeof(expr)-strlen(expr)-1);
        strncat(expr, cmd->argv[i], sizeof(expr)-strlen(expr)-1);
    }

    CalcParser p = { expr, 0 };
    double result = calc_expr(&p);

    if (result == (long long)result && result >= -1e15 && result <= 1e15)
        printf("\x1b[1m\x1b[38;5;226m%s\x1b[38;5;245m = \x1b[38;5;82m%lld\x1b[0m\n",
               expr, (long long)result);
    else
        printf("\x1b[1m\x1b[38;5;226m%s\x1b[38;5;245m = \x1b[38;5;82m%g\x1b[0m\n",
               expr, result);
    return 0;
}

#define FIG_H 5
#define FIG_CHARS 38

static const char *FIG_DATA[FIG_CHARS][FIG_H] = {
 {" ### ","#   #","#####","#   #","#   #"},
 {"#### ","#   #","#### ","#   #","#### "},
 {" ####","#    ","#    ","#    "," ####"},
 {"#### ","#   #","#   #","#   #","#### "},
 {"#####","#    ","#### ","#    ","#####"},
 {"#####","#    ","#### ","#    ","#    "},
 {" ####","#    ","#  ##","#   #"," ####"},
 {"#   #","#   #","#####","#   #","#   #"},
 {"#####","  #  ","  #  ","  #  ","#####"},
 {"#####","   # ","   # ","#  # "," ##  "},
 {"#   #","#  # ","###  ","#  # ","#   #"},
 {"#    ","#    ","#    ","#    ","#####"},
 {"#   #","## ##","# # #","#   #","#   #"},
 {"#   #","##  #","# # #","#  ##","#   #"},
 {" ### ","#   #","#   #","#   #"," ### "},
 {"#### ","#   #","#### ","#    ","#    "},
 {" ### ","#   #","# # #","#  # "," ## #"},
 {"#### ","#   #","#### ","#  # ","#   #"},
 {" ####","#    "," ### ","    #","#### "},
 {"#####","  #  ","  #  ","  #  ","  #  "},
 {"#   #","#   #","#   #","#   #"," ### "},
 {"#   #","#   #","#   #"," # # ","  #  "},
 {"#   #","#   #","# # #","## ##","#   #"},
 {"#   #"," # # ","  #  "," # # ","#   #"},
 {"#   #"," # # ","  #  ","  #  ","  #  "},
 {"#####","   # ","  #  "," #   ","#####"},
 {" ### ","#  ##","# # #","## #"," ### "},
 {" ##  ","  #  ","  #  ","  #  ","#####"},
 {" ### ","#   #","  ## "," #   ","#####"},
 {"#### ","    #"," ### ","    #","#### "},
 {"#   #","#   #","#####","    #","    #"},
 {"#####","#    ","#### ","    #","#### "},
 {" ### ","#    ","#### ","#   #"," ### "},
 {"#####","    #","   # ","  #  ","  #  "},
 {" ### ","#   #"," ### ","#   #"," ### "},
 {" ### ","#   #"," ####","    #"," ### "},
 {"     ","     ","     ","     ","     "},
 {"  #  ","  #  ","  #  ","     ","  #  "},
 {" ### ","    #","  ## ","     ","  #  "},
};

static int fig_idx(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == ' ')  return 36;
    if (c == '!')  return 37;
    if (c == '?')  return FIG_CHARS - 1;
    return 36;
}

static const char *FIG_COLS[] = {
    "\x1b[38;5;51m", "\x1b[38;5;82m", "\x1b[38;5;226m",
    "\x1b[38;5;207m","\x1b[38;5;196m","\x1b[38;5;208m",
};
#define FIG_NCOLS 6

static int b_figlet(Cmd *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "usage: figlet <text>\n");
        return 1;
    }

    char text[256] = "";
    for (int i = 1; i < cmd->argc; i++) {
        if (i > 1) strncat(text, " ", sizeof(text)-strlen(text)-1);
        strncat(text, cmd->argv[i], sizeof(text)-strlen(text)-1);
    }
    int len = strlen(text);

    printf("\n");
    for (int row = 0; row < FIG_H; row++) {
        printf("  ");
        for (int ci = 0; ci < len; ci++) {
            int idx = fig_idx(text[ci]);
            const char *col = FIG_COLS[ci % FIG_NCOLS];
            const char *line = FIG_DATA[idx][row];
            for (int k = 0; line[k]; k++) {
                if (line[k] == '#')
                    printf("%s\x1b[1m█\x1b[0m", col);
                else
                    printf(" ");
            }
            printf(" ");
        }
        printf("\n");
    }
    printf("\n");
    return 0;
}
