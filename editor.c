
#define ED_MAXLINES  65536
#define ED_MAXLINEW  4096

typedef struct {
    char  *buf;
    int    len;
    int    cap;
} EdLine;

typedef struct {
    EdLine  lines[ED_MAXLINES];
    int     nlines;

    int     cx, cy;
    int     scroll_row;
    int     scroll_col;

    char    filename[512];
    int     dirty;

    char   *cutbuf;
    int     cutlen;

    char    search[256];

    int     rows, cols;

    char    status[256];
    time_t  status_time;
} Editor;

static Editor E;

static void ed_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        E.rows = ws.ws_row - 3;
        E.cols = ws.ws_col;
    } else {
        E.rows = 22; E.cols = 80;
    }
}

static void ed_raw_on(struct termios *saved) {
    tcgetattr(STDIN_FILENO, saved);
    struct termios raw = *saved;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |=  CS8;
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void ed_raw_off(struct termios *saved) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, saved);
}

static void ed_write(const char *s, int len) {
    write(STDOUT_FILENO, s, len);
}
static void ed_puts(const char *s) { ed_write(s, strlen(s)); }

static void ed_line_ensure(EdLine *l, int needed) {
    if (needed + 1 > l->cap) {
        l->cap = needed + 128;
        l->buf = realloc(l->buf, l->cap);
    }
}

static void ed_line_init(EdLine *l, const char *s, int len) {
    l->cap = 0; l->buf = NULL;
    ed_line_ensure(l, len);
    if (s && len) memcpy(l->buf, s, len);
    l->buf[len] = '\0';
    l->len = len;
}

static void ed_line_insert(EdLine *l, int at, char c) {
    ed_line_ensure(l, l->len + 1);
    memmove(l->buf + at + 1, l->buf + at, l->len - at);
    l->buf[at] = c;
    l->len++;
    l->buf[l->len] = '\0';
}

static void ed_line_delete(EdLine *l, int at) {
    if (at < 0 || at >= l->len) return;
    memmove(l->buf + at, l->buf + at + 1, l->len - at);
    l->len--;
    l->buf[l->len] = '\0';
}

static void ed_insert_line(int at) {
    if (E.nlines >= ED_MAXLINES) return;
    memmove(&E.lines[at + 1], &E.lines[at], (E.nlines - at) * sizeof(EdLine));
    ed_line_init(&E.lines[at], NULL, 0);
    E.nlines++;
}

static void ed_delete_line(int at) {
    if (E.nlines <= 1) { E.lines[0].len = 0; E.lines[0].buf[0] = '\0'; return; }
    free(E.lines[at].buf);
    memmove(&E.lines[at], &E.lines[at + 1], (E.nlines - at - 1) * sizeof(EdLine));
    E.nlines--;
}

static void ed_load(const char *path) {

    for (int i = 0; i < E.nlines; i++) free(E.lines[i].buf);
    E.nlines = 0;

    FILE *f = fopen(path, "r");
    if (!f) {

        ed_line_init(&E.lines[0], NULL, 0);
        E.nlines = 1;
        snprintf(E.status, sizeof(E.status), "New file: %s", path);
        E.status_time = time(NULL);
        return;
    }

    char line[ED_MAXLINEW];
    while (fgets(line, sizeof(line), f) && E.nlines < ED_MAXLINES) {
        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') len--;
        ed_line_init(&E.lines[E.nlines++], line, len);
    }
    fclose(f);

    if (E.nlines == 0) {
        ed_line_init(&E.lines[0], NULL, 0);
        E.nlines = 1;
    }
    snprintf(E.status, sizeof(E.status), "Opened: %s  (%d lines)", path, E.nlines);
    E.status_time = time(NULL);
}

static int ed_save(void) {
    if (!E.filename[0]) {
        snprintf(E.status, sizeof(E.status), "No filename — use Ctrl-S after naming");
        E.status_time = time(NULL);
        return 0;
    }
    FILE *f = fopen(E.filename, "w");
    if (!f) {
        snprintf(E.status, sizeof(E.status), "Error saving: %s", strerror(errno));
        E.status_time = time(NULL);
        return 0;
    }
    int total = 0;
    for (int i = 0; i < E.nlines; i++) {
        fputs(E.lines[i].buf, f);
        fputc('\n', f);
        total += E.lines[i].len + 1;
    }
    fclose(f);
    E.dirty = 0;
    snprintf(E.status, sizeof(E.status),
             "Saved: %s  (%d lines, %d bytes)", E.filename, E.nlines, total);
    E.status_time = time(NULL);
    return 1;
}

#define ED_ABUF_INIT 4096
typedef struct { char *b; int len, cap; } ABuf;

static void ab_append(ABuf *ab, const char *s, int len) {
    if (ab->len + len >= ab->cap) {
        ab->cap = ab->len + len + ED_ABUF_INIT;
        ab->b = realloc(ab->b, ab->cap);
    }
    memcpy(ab->b + ab->len, s, len);
    ab->len += len;
}
static void ab_appends(ABuf *ab, const char *s) { ab_append(ab, s, strlen(s)); }
static void ab_free(ABuf *ab) { free(ab->b); }

static void ed_render(void) {
    ed_term_size();
    ABuf ab = {0};

    ab_appends(&ab, "\x1b[?25l\x1b[H");

    {
        char hdr[256];
        const char *fname = E.filename[0] ? E.filename : "[No Name]";
        const char *mod   = E.dirty ? " [+]" : "";
        snprintf(hdr, sizeof(hdr),
                 " Triumph Editor  |  %s%s  |  Ln %d/%d  Col %d",
                 fname, mod, E.cy + 1, E.nlines, E.cx + 1);
        ab_appends(&ab, "\x1b[48;5;17m\x1b[38;5;51m\x1b[1m");
        ab_append (&ab, hdr, strlen(hdr));

        int pad = E.cols - (int)strlen(hdr);
        for (int i = 0; i < pad && i < 256; i++) ab_append(&ab, " ", 1);
        ab_appends(&ab, "\x1b[0m\r\n");
    }

    if (E.cy < E.scroll_row) E.scroll_row = E.cy;
    if (E.cy >= E.scroll_row + E.rows) E.scroll_row = E.cy - E.rows + 1;
    if (E.cx < E.scroll_col) E.scroll_col = E.cx;
    if (E.cx >= E.scroll_col + E.cols - 5) E.scroll_col = E.cx - E.cols + 6;

    for (int row = 0; row < E.rows; row++) {
        int fileline = row + E.scroll_row;

        char lnum[8];
        if (fileline < E.nlines) {
            snprintf(lnum, sizeof(lnum), "%4d ", fileline + 1);
            ab_appends(&ab, "\x1b[38;5;240m");
            ab_append (&ab, lnum, strlen(lnum));
            ab_appends(&ab, "\x1b[0m");
        } else {
            ab_appends(&ab, "\x1b[38;5;237m~    \x1b[0m");
        }

        if (fileline < E.nlines) {
            EdLine *l = &E.lines[fileline];
            int start = E.scroll_col;
            int end   = start + E.cols - 5;
            if (end > l->len) end = l->len;
            if (start < l->len) {
                ab_append(&ab, l->buf + start, end - start);
            }
        }

        ab_appends(&ab, "\x1b[K\r\n");
    }

    {
        ab_appends(&ab, "\x1b[48;5;236m\x1b[38;5;245m");
        char msg[256] = "";
        if (E.status[0] && time(NULL) - E.status_time < 5)
            strncpy(msg, E.status, sizeof(msg) - 1);
        ab_append(&ab, msg, strlen(msg));
        int pad = E.cols - (int)strlen(msg);
        for (int i = 0; i < pad && i < 512; i++) ab_append(&ab, " ", 1);
        ab_appends(&ab, "\x1b[0m\r\n");
    }

    ab_appends(&ab,
        "\x1b[48;5;17m\x1b[38;5;51m"
        " ^S Save  ^X Exit  ^K Cut  ^U Paste  ^W Find  ^N Next  ^A Home  ^E End"
        "\x1b[0m");
    {

        int used = (int)strlen(" ^S Save  ^X Exit  ^K Cut  ^U Paste  ^W Find  ^N Next  ^A Home  ^E End");
        for (int i = used; i < E.cols; i++) ab_append(&ab, " ", 1);
    }

    {
        char cur[32];
        int screen_row = (E.cy - E.scroll_row) + 2;
        int screen_col = (E.cx - E.scroll_col) + 6;
        snprintf(cur, sizeof(cur), "\x1b[%d;%dH", screen_row, screen_col);
        ab_appends(&ab, cur);
    }

    ab_appends(&ab, "\x1b[?25h");

    ed_write(ab.b, ab.len);
    ab_free(&ab);
}

#define ED_KEY_ARROW_UP    1000
#define ED_KEY_ARROW_DOWN  1001
#define ED_KEY_ARROW_LEFT  1002
#define ED_KEY_ARROW_RIGHT 1003
#define ED_KEY_HOME        1004
#define ED_KEY_END         1005
#define ED_KEY_PGUP        1006
#define ED_KEY_PGDN        1007
#define ED_KEY_DEL         1008

static int ed_read_key(void) {
    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) != 1);

    if (c == '\x1b') {
        unsigned char s[3];
        if (read(STDIN_FILENO, &s[0], 1) != 1) return '\x1b';
        if (s[0] != '[') return '\x1b';
        if (read(STDIN_FILENO, &s[1], 1) != 1) return '\x1b';
        if (s[1] >= '0' && s[1] <= '9') {
            unsigned char t;
            if (read(STDIN_FILENO, &t, 1) != 1) return '\x1b';
            if (t == '~') {
                switch (s[1]) {
                    case '1': return ED_KEY_HOME;
                    case '4': return ED_KEY_END;
                    case '5': return ED_KEY_PGUP;
                    case '6': return ED_KEY_PGDN;
                    case '3': return ED_KEY_DEL;
                }
            }
        } else {
            switch (s[1]) {
                case 'A': return ED_KEY_ARROW_UP;
                case 'B': return ED_KEY_ARROW_DOWN;
                case 'C': return ED_KEY_ARROW_RIGHT;
                case 'D': return ED_KEY_ARROW_LEFT;
                case 'H': return ED_KEY_HOME;
                case 'F': return ED_KEY_END;
            }
        }
        return '\x1b';
    }
    return (int)c;
}

static void ed_handle_mouse(int ox, int scroll_row, int scroll_col) {

    char buf[32]; int bi = 0;
    while (bi < 31) {
        fd_set fds; struct timeval tv = {0, 50000};
        FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO+1,&fds,NULL,NULL,&tv) <= 0) break;
        char c; if (read(STDIN_FILENO,&c,1) != 1) break;
        buf[bi++] = c;
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')) break;
    }
    buf[bi] = 0;

    int btn, mx, my;
    if (sscanf(buf, "%d;%d;%d", &btn, &mx, &my) == 3) {

        int fy = (my - 2) + scroll_row;
        int fx = (mx - 6) + scroll_col;
        if (fy >= 0 && fy < E.nlines) {
            E.cy = fy;
            if (fx >= 0 && fx <= E.lines[E.cy].len) E.cx = fx;
            else if (fx < 0) E.cx = 0;
            else E.cx = E.lines[E.cy].len;
        }
    }
    (void)ox;
}

static void ed_clamp_cx(void) {
    int rowlen = (E.cy < E.nlines) ? E.lines[E.cy].len : 0;
    if (E.cx > rowlen) E.cx = rowlen;
    if (E.cx < 0)      E.cx = 0;
}

static void ed_move(int key) {
    switch (key) {
        case ED_KEY_ARROW_LEFT:
            if (E.cx > 0) E.cx--;
            else if (E.cy > 0) { E.cy--; E.cx = E.lines[E.cy].len; }
            break;
        case ED_KEY_ARROW_RIGHT:
            if (E.cy < E.nlines) {
                if (E.cx < E.lines[E.cy].len) E.cx++;
                else if (E.cy < E.nlines - 1) { E.cy++; E.cx = 0; }
            }
            break;
        case ED_KEY_ARROW_UP:
            if (E.cy > 0) E.cy--;
            ed_clamp_cx();
            break;
        case ED_KEY_ARROW_DOWN:
            if (E.cy < E.nlines - 1) E.cy++;
            ed_clamp_cx();
            break;
        case ED_KEY_HOME:  case 1:
            E.cx = 0; break;
        case ED_KEY_END:   case 5:
            if (E.cy < E.nlines) E.cx = E.lines[E.cy].len; break;
        case ED_KEY_PGUP:  case 2:
            E.cy -= E.rows;
            if (E.cy < 0) E.cy = 0;
            ed_clamp_cx();
            break;
        case ED_KEY_PGDN:  case 6:
            E.cy += E.rows;
            if (E.cy >= E.nlines) E.cy = E.nlines - 1;
            ed_clamp_cx();
            break;
    }
}

static void ed_insert_char(char c) {
    if (E.cy >= E.nlines) return;
    ed_line_insert(&E.lines[E.cy], E.cx, c);
    E.cx++;
    E.dirty = 1;
}

static void ed_insert_newline(void) {
    EdLine *cur = &E.lines[E.cy];
    int rest_len = cur->len - E.cx;
    char rest[ED_MAXLINEW];
    if (rest_len > 0) {
        memcpy(rest, cur->buf + E.cx, rest_len);
        cur->len = E.cx;
        cur->buf[cur->len] = '\0';
    }
    ed_insert_line(E.cy + 1);
    if (rest_len > 0) {
        EdLine *nl = &E.lines[E.cy + 1];
        ed_line_ensure(nl, rest_len);
        memcpy(nl->buf, rest, rest_len);
        nl->buf[rest_len] = '\0';
        nl->len = rest_len;
    }
    E.cy++;
    E.cx = 0;
    E.dirty = 1;
}

static void ed_backspace(void) {
    if (E.cx == 0 && E.cy == 0) return;
    if (E.cx == 0) {

        EdLine *prev = &E.lines[E.cy - 1];
        EdLine *cur  = &E.lines[E.cy];
        E.cx = prev->len;
        ed_line_ensure(prev, prev->len + cur->len);
        memcpy(prev->buf + prev->len, cur->buf, cur->len);
        prev->len += cur->len;
        prev->buf[prev->len] = '\0';
        ed_delete_line(E.cy);
        E.cy--;
    } else {
        ed_line_delete(&E.lines[E.cy], E.cx - 1);
        E.cx--;
    }
    E.dirty = 1;
}

static void ed_delete_char(void) {
    if (E.cy >= E.nlines) return;
    EdLine *cur = &E.lines[E.cy];
    if (E.cx < cur->len) {
        ed_line_delete(cur, E.cx);
    } else if (E.cy < E.nlines - 1) {

        EdLine *next = &E.lines[E.cy + 1];
        ed_line_ensure(cur, cur->len + next->len);
        memcpy(cur->buf + cur->len, next->buf, next->len);
        cur->len += next->len;
        cur->buf[cur->len] = '\0';
        ed_delete_line(E.cy + 1);
    }
    E.dirty = 1;
}

static void ed_cut_line(void) {
    if (E.cy >= E.nlines) return;
    free(E.cutbuf);
    E.cutbuf = strdup(E.lines[E.cy].buf);
    E.cutlen = E.lines[E.cy].len;
    ed_delete_line(E.cy);
    if (E.cy >= E.nlines) E.cy = E.nlines - 1;
    E.cx = 0;
    E.dirty = 1;
    snprintf(E.status, sizeof(E.status), "Line cut");
    E.status_time = time(NULL);
}

static void ed_paste(void) {
    if (!E.cutbuf) return;
    ed_insert_line(E.cy);
    EdLine *nl = &E.lines[E.cy];
    ed_line_ensure(nl, E.cutlen);
    memcpy(nl->buf, E.cutbuf, E.cutlen);
    nl->buf[E.cutlen] = '\0';
    nl->len = E.cutlen;
    E.cy++;
    E.cx = 0;
    E.dirty = 1;
    snprintf(E.status, sizeof(E.status), "Pasted");
    E.status_time = time(NULL);
}

static void ed_prompt(const char *prompt, char *buf, int buflen,
                      struct termios *saved) {
    ed_raw_off(saved);

    printf("\x1b[%d;1H\x1b[K\x1b[38;5;226m%s\x1b[0m ", E.rows + 2, prompt);
    fflush(stdout);
    if (fgets(buf, buflen, stdin)) {
        buf[strcspn(buf, "\n")] = '\0';
    }
    ed_raw_on(saved);
}

static void ed_find_next(int start_row, int start_col) {
    if (!E.search[0]) return;
    for (int i = 0; i < E.nlines; i++) {
        int row = (start_row + i) % E.nlines;
        int col_start = (i == 0) ? start_col : 0;
        char *found = strstr(E.lines[row].buf + col_start, E.search);
        if (found) {
            E.cy = row;
            E.cx = (int)(found - E.lines[row].buf);
            snprintf(E.status, sizeof(E.status), "Found: '%s'", E.search);
            E.status_time = time(NULL);
            return;
        }
    }
    snprintf(E.status, sizeof(E.status), "Not found: '%s'", E.search);
    E.status_time = time(NULL);
}

static int b_edit(Cmd *cmd) {

    memset(&E, 0, sizeof(E));
    E.nlines = 0;
    E.cutbuf = NULL;

    if (cmd->argc > 1) {
        strncpy(E.filename, cmd->argv[1], sizeof(E.filename) - 1);
        ed_load(E.filename);
    } else {
        ed_line_init(&E.lines[0], NULL, 0);
        E.nlines = 1;
        snprintf(E.status, sizeof(E.status),
                 "New buffer — Ctrl-S to save, Ctrl-X to exit");
        E.status_time = time(NULL);
    }

    ed_term_size();

    struct termios saved;
    ed_raw_on(&saved);

    ed_puts("\x1b[2J\x1b[H");
    ed_puts("\x1b[?1000h\x1b[?1002h\x1b[?1015h\x1b[?1006h");

    int running_ed = 1;
    while (running_ed) {
        ed_render();
        int key = ed_read_key();

        switch (key) {

            case ED_KEY_ARROW_UP:
            case ED_KEY_ARROW_DOWN:
            case ED_KEY_ARROW_LEFT:
            case ED_KEY_ARROW_RIGHT:
            case ED_KEY_HOME:
            case ED_KEY_END:
            case ED_KEY_PGUP:
            case ED_KEY_PGDN:
            case 1:
            case 5:
            case 2:
            case 6:
                ed_move(key);
                break;

            case '\r':
            case '\n':
                ed_insert_newline();
                break;

            case 127:
            case 8:
                ed_backspace();
                break;

            case ED_KEY_DEL:
                ed_delete_char();
                break;

            case '\t':

                for (int i = 0; i < 4; i++) ed_insert_char(' ');
                break;

            case 11:
                ed_cut_line();
                break;

            case 21:
                ed_paste();
                break;

            case 23: {
                char tmp[256] = "";
                ed_prompt("Search:", tmp, sizeof(tmp), &saved);
                if (tmp[0]) strncpy(E.search, tmp, sizeof(E.search) - 1);
                ed_find_next(E.cy, E.cx + 1);
                break;
            }
            case 14:
                ed_find_next(E.cy, E.cx + 1);
                break;

            case 19: {
                if (!E.filename[0]) {
                    char tmp[512] = "";
                    ed_prompt("Save as:", tmp, sizeof(tmp), &saved);
                    if (tmp[0]) strncpy(E.filename, tmp, sizeof(E.filename) - 1);
                }
                if (E.filename[0]) ed_save();
                break;
            }

            case 12:
                ed_puts("\x1b[2J");
                break;

            case 24: {
                if (E.dirty) {
                    char tmp[8] = "";
                    ed_prompt("Unsaved changes. Save? (y/n):", tmp, sizeof(tmp), &saved);
                    if (tmp[0] == 'y' || tmp[0] == 'Y') {
                        if (!E.filename[0]) {
                            char fn[512] = "";
                            ed_prompt("Save as:", fn, sizeof(fn), &saved);
                            if (fn[0]) strncpy(E.filename, fn, sizeof(E.filename) - 1);
                        }
                        ed_save();
                    }
                }
                running_ed = 0;
                break;
            }

            default:
                if (key >= 32 && key < 127) {
                    ed_insert_char((char)key);
                }
                break;
        }
    }

    ed_raw_off(&saved);

    ed_puts("\x1b[?1006l\x1b[?1015l\x1b[?1002l\x1b[?1000l");
    ed_puts("\x1b[2J\x1b[H");

    for (int i = 0; i < E.nlines; i++) free(E.lines[i].buf);
    free(E.cutbuf);

    return 0;
}
