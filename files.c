/*
 * Triumph file explorer.
 * Arrow keys to navigate, Enter to descend into directory or read file.
 * Shift+E to edit, Shift+R to read, Shift+D to delete.
 */

#define FX_BG  "\x1b[48;5;17m"
#define FX_FG  "\x1b[38;5;51m"
#define FX_HI  "\x1b[48;5;51m\x1b[38;5;17m"
#define FX_DIM "\x1b[38;5;33m"
#define FX_DIM2 "\x1b[38;5;67m"
#define FX_YEL "\x1b[38;5;226m"
#define FX_GRN "\x1b[38;5;82m"
#define FX_RED "\x1b[38;5;196m"

typedef struct {
    char  name[256];
    int   is_dir;
    long  size;
} FxEnt;

#define FX_MAX 512

static int fx_compare(const void *a, const void *b) {
    const FxEnt *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcmp(x->name, y->name);
}

static int fx_load(const char *dir, FxEnt *ents, int max_ents) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;

    /* Always include ".." unless we're at "/" */
    if (strcmp(dir, "/") != 0 && n < max_ents) {
        strcpy(ents[n].name, "..");
        ents[n].is_dir = 1;
        ents[n].size = 0;
        n++;
    }

    struct dirent *e;
    while ((e = readdir(d)) && n < max_ents) {
        if (e->d_name[0]=='.' && (e->d_name[1]==0 || (e->d_name[1]=='.' && e->d_name[2]==0)))
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) < 0) continue;
        strncpy(ents[n].name, e->d_name, 255);
        ents[n].name[255] = 0;
        ents[n].is_dir = S_ISDIR(st.st_mode);
        ents[n].size = (long)st.st_size;
        n++;
    }
    closedir(d);
    qsort(ents + (strcmp(dir,"/")?1:0), n - (strcmp(dir,"/")?1:0), sizeof(FxEnt), fx_compare);
    return n;
}

static void fx_w(const char *s) { write(1, s, strlen(s)); }
static void fx_at(int r, int c) { char b[24]; snprintf(b,24,"\x1b[%d;%dH",r,c); fx_w(b); }

static int fx_readkey(int blocking) {
    if (!blocking) {
        struct termios cur; tcgetattr(0,&cur);
        struct termios tmp = cur;
        tmp.c_cc[VMIN]=0; tmp.c_cc[VTIME]=1;
        tcsetattr(0,TCSANOW,&tmp);
        unsigned char c;
        int r = read(0,&c,1);
        tcsetattr(0,TCSANOW,&cur);
        return r > 0 ? c : 0;
    }
    unsigned char c;
    if (read(0,&c,1)<=0) return 0;
    if (c==0x1b) {
        struct termios cur; tcgetattr(0,&cur);
        struct termios tmp = cur;
        tmp.c_cc[VMIN]=0; tmp.c_cc[VTIME]=1;
        tcsetattr(0,TCSANOW,&tmp);
        unsigned char seq[3]; int n = read(0,seq,3);
        tcsetattr(0,TCSANOW,&cur);
        if (n<=0) return 27;  /* bare Esc */
        if (seq[0]=='[') {
            if (seq[1]=='A') return 0x101;  /* up */
            if (seq[1]=='B') return 0x102;  /* down */
            if (seq[1]=='C') return 0x103;  /* right */
            if (seq[1]=='D') return 0x104;  /* left */
        }
        return 27;
    }
    return c;
}

static void fx_paint_bg(int rows, int cols) {
    fx_w(FX_BG);
    fx_at(1,1);
    for (int r=0;r<rows;r++) {
        for (int c=0;c<cols;c++) fx_w(" ");
        if (r<rows-1) fx_w("\r\n");
    }
}

static void fx_status(int rows, int cols, const char *msg, const char *colour) {
    fx_at(rows-2, 2);
    fx_w(colour);
    char pad[256]; snprintf(pad,sizeof(pad),"%-*s", cols-4, msg);
    fx_w(pad);
}

static void fx_draw(const char *cwd, FxEnt *ents, int n, int sel, int top,
                    int rows, int cols, const char *status, const char *status_col) {
    fx_paint_bg(rows, cols);

    /* Title */
    fx_at(2, 2);
    fx_w(FX_YEL"\x1b[1m"); fx_w("── FILE EXPLORER ──"); fx_w("\x1b[22m");

    /* CWD */
    fx_at(3, 2);
    fx_w(FX_FG"path: "FX_DIM2); fx_w(cwd);

    /* Top divider */
    fx_at(4, 1); fx_w(FX_DIM);
    for (int c=0;c<cols;c++) fx_w("─");

    /* List */
    int list_top = 5;
    int list_h   = rows - list_top - 4;
    int visible  = list_h;
    if (sel < top) top = sel;
    if (sel >= top + visible) top = sel - visible + 1;

    for (int i=0; i<visible && top+i<n; i++) {
        int idx = top + i;
        FxEnt *e = &ents[idx];
        fx_at(list_top + i, 2);
        if (idx == sel) {
            fx_w(FX_HI"\x1b[1m");
            char line[512];
            char sz[24];
            if (e->is_dir) snprintf(sz, sizeof(sz), "<DIR>");
            else if (e->size < 1024)        snprintf(sz, sizeof(sz), "%ldB", e->size);
            else if (e->size < 1024*1024)   snprintf(sz, sizeof(sz), "%ldK", e->size/1024);
            else                            snprintf(sz, sizeof(sz), "%ldM", e->size/(1024*1024));
            snprintf(line, sizeof(line), " %s %-*s %10s ",
                     e->is_dir?"▶":" ", cols-20, e->name, sz);
            fx_w(line);
            fx_w("\x1b[22m"); fx_w(FX_BG FX_FG);
        } else {
            if (e->is_dir) fx_w(FX_FG"\x1b[1m"); else fx_w(FX_DIM2);
            char line[512];
            char sz[24];
            if (e->is_dir) snprintf(sz, sizeof(sz), "<DIR>");
            else if (e->size < 1024)        snprintf(sz, sizeof(sz), "%ldB", e->size);
            else if (e->size < 1024*1024)   snprintf(sz, sizeof(sz), "%ldK", e->size/1024);
            else                            snprintf(sz, sizeof(sz), "%ldM", e->size/(1024*1024));
            snprintf(line, sizeof(line), " %s %-*s %10s ",
                     e->is_dir?"▶":" ", cols-20, e->name, sz);
            fx_w(line);
            fx_w("\x1b[22m");
        }
    }

    /* Status line */
    if (status) fx_status(rows, cols, status, status_col?status_col:FX_FG);

    /* Footer */
    fx_at(rows-1, 1); fx_w(FX_DIM);
    for (int c=0;c<cols;c++) fx_w("─");
    fx_at(rows, 2);
    fx_w(FX_FG"↑↓ "FX_DIM"nav  "FX_FG"ENTER "FX_DIM"open  "FX_FG"R "FX_DIM"read  "FX_FG"E "FX_DIM"edit  "FX_FG"D "FX_DIM"delete  "FX_FG"ESC "FX_DIM"back");
    fflush(stdout);
}

static void fx_read_file(const char *path, int rows, int cols) {
    fx_w("\x1b[0m\x1b[2J\x1b[H");
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("\nCannot open: %s\nPress any key...", strerror(errno));
        fflush(stdout);
        unsigned char c; read(0,&c,1);
        return;
    }
    /* Title */
    printf("\x1b[1m\x1b[38;5;51m── %s ──\x1b[0m\n", path);
    printf("\x1b[38;5;33m");
    for (int i=0; i<cols-1; i++) putchar('-');
    printf("\x1b[0m\n");

    /* Body — show first ~rows-6 lines */
    char buf[4096]; int n;
    int line_count = 0;
    int max_lines = rows - 6;
    while ((n = read(fd, buf, sizeof(buf))) > 0 && line_count < max_lines) {
        for (int i=0; i<n && line_count < max_lines; i++) {
            putchar(buf[i]);
            if (buf[i] == '\n') line_count++;
        }
    }
    close(fd);
    printf("\n\x1b[38;5;33m");
    for (int i=0; i<cols-1; i++) putchar('-');
    printf("\n\x1b[38;5;226mPress any key to return...\x1b[0m");
    fflush(stdout);
    unsigned char c; read(0,&c,1);
}

static int b_files(Cmd *c) { (void)c;
    struct termios old, raw;
    tcgetattr(0,&old); raw=old;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    tcsetattr(0,TCSANOW,&raw);

    struct winsize ws; ioctl(0,TIOCGWINSZ,&ws);
    int rows = ws.ws_row?ws.ws_row:24;
    int cols = ws.ws_col?ws.ws_col:80;

    fx_w("\x1b[?25l\x1b[2J");

    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");

    FxEnt ents[FX_MAX];
    int n = fx_load(cwd, ents, FX_MAX);
    if (n < 0) { strcpy(cwd, "/"); n = fx_load(cwd, ents, FX_MAX); }

    int sel = 0, top = 0;
    char status[256] = "";
    const char *status_col = NULL;

    while (1) {
        fx_draw(cwd, ents, n, sel, top, rows, cols,
                status[0]?status:NULL, status_col);
        status[0]=0; status_col=NULL;

        int k = fx_readkey(1);

        if (k==0x101 || k=='k') { if (sel>0) sel--; }
        else if (k==0x102 || k=='j') { if (sel<n-1) sel++; }
        else if (k==13 || k==10) {
            FxEnt *e = &ents[sel];
            if (e->is_dir) {
                /* Navigate */
                char newcwd[1024];
                if (strcmp(e->name, "..") == 0) {
                    /* parent */
                    strcpy(newcwd, cwd);
                    char *slash = strrchr(newcwd, '/');
                    if (slash) {
                        if (slash == newcwd) slash[1] = 0;
                        else *slash = 0;
                    }
                } else {
                    if (strcmp(cwd, "/") == 0)
                        snprintf(newcwd, sizeof(newcwd), "/%s", e->name);
                    else
                        snprintf(newcwd, sizeof(newcwd), "%s/%s", cwd, e->name);
                }
                strncpy(cwd, newcwd, sizeof(cwd)-1);
                cwd[sizeof(cwd)-1] = 0;
                n = fx_load(cwd, ents, FX_MAX);
                if (n < 0) { strcpy(cwd, "/"); n = fx_load(cwd, ents, FX_MAX); }
                sel = 0; top = 0;
            } else {
                /* Open in read mode (lower r and Enter both) */
                char full[2048];
                snprintf(full, sizeof(full), "%s/%s", cwd, e->name);
                tcsetattr(0,TCSANOW,&old);
                fx_w("\x1b[?25h");
                fx_read_file(full, rows, cols);
                tcsetattr(0,TCSANOW,&raw);
                fx_w("\x1b[?25l\x1b[2J");
            }
        }
        else if (k=='R') {
            FxEnt *e = &ents[sel];
            if (!e->is_dir) {
                char full[2048];
                snprintf(full, sizeof(full), "%s/%s", cwd, e->name);
                tcsetattr(0,TCSANOW,&old);
                fx_w("\x1b[?25h");
                fx_read_file(full, rows, cols);
                tcsetattr(0,TCSANOW,&raw);
                fx_w("\x1b[?25l\x1b[2J");
            } else {
                strcpy(status, "Cannot read a directory");
                status_col = FX_RED;
            }
        }
        else if (k=='E') {
            FxEnt *e = &ents[sel];
            if (!e->is_dir) {
                char full[2048];
                snprintf(full, sizeof(full), "%s/%s", cwd, e->name);
                tcsetattr(0,TCSANOW,&old);
                fx_w("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
                /* Run editor */
                char line[2200];
                snprintf(line, sizeof(line), "edit %s", full);
                run_line(line);
                tcsetattr(0,TCSANOW,&raw);
                fx_w("\x1b[?25l\x1b[2J");
            } else {
                strcpy(status, "Cannot edit a directory");
                status_col = FX_RED;
            }
        }
        else if (k=='D') {
            FxEnt *e = &ents[sel];
            if (strcmp(e->name, "..") == 0) {
                strcpy(status, "Cannot delete '..'");
                status_col = FX_RED;
            } else {
                /* Confirm */
                fx_status(rows, cols, "Delete? Y to confirm, anything else to cancel", FX_YEL);
                fflush(stdout);
                int conf = fx_readkey(1);
                if (conf=='Y' || conf=='y') {
                    char full[2048];
                    snprintf(full, sizeof(full), "%s/%s", cwd, e->name);
                    int rc;
                    if (e->is_dir) rc = rmdir(full);
                    else           rc = unlink(full);
                    if (rc < 0) {
                        snprintf(status, sizeof(status), "Delete failed: %s", strerror(errno));
                        status_col = FX_RED;
                    } else {
                        snprintf(status, sizeof(status), "Deleted: %s", e->name);
                        status_col = FX_GRN;
                        n = fx_load(cwd, ents, FX_MAX);
                        if (sel >= n) sel = n-1;
                        if (sel < 0) sel = 0;
                    }
                } else {
                    strcpy(status, "Cancelled");
                    status_col = FX_DIM;
                }
            }
        }
        else if (k==27 || k=='q' || k=='Q') break;
    }

    fx_w("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    tcsetattr(0,TCSANOW,&old);
    return 0;
}
