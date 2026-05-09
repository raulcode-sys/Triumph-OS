#include <mbedtls/sha256.h>

#define LG_BG   "\x1b[48;5;17m"
#define LG_FG   "\x1b[38;5;51m"
#define LG_DIM  "\x1b[38;5;33m"
#define LG_DIM2 "\x1b[38;5;67m"
#define LG_YEL  "\x1b[38;5;226m"
#define LG_GRN  "\x1b[38;5;82m"
#define LG_RED  "\x1b[38;5;196m"

#define PERSIST_MOUNT  "/persist"
#define USERS_DB       "/persist/users.db"
#define HOMES_DIR      "/persist/home"
#define MAX_USERS      32
#define MAX_NAME       32
#define MAX_HASH       65   
#define MAX_SALT       17   

typedef struct {
    char name[MAX_NAME];
    char salt[MAX_SALT];
    char hash[MAX_HASH];  
} LgUser;

static LgUser g_users[MAX_USERS];
static int    g_nusers = 0;
static char   g_current_user[MAX_NAME] = "root";
static int    g_persist_ok = 0;

static void lg_w(const char *s) { write(1, s, strlen(s)); }
static void lg_at(int r, int c) { char b[24]; snprintf(b,24,"\x1b[%d;%dH",r,c); lg_w(b); }

static void lg_hex(const unsigned char *buf, int len, char *out) {
    static const char h[] = "0123456789abcdef";
    for (int i=0; i<len; i++) {
        out[i*2]   = h[buf[i]>>4];
        out[i*2+1] = h[buf[i]&0xf];
    }
    out[len*2] = 0;
}

static void lg_hash(const char *salt, const char *pass, char *out_hex) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s", salt, pass);
    unsigned char digest[32];
    mbedtls_sha256((const unsigned char*)buf, strlen(buf), digest, 0);
    lg_hex(digest, 32, out_hex);
}

static void lg_make_salt(char *out) {
    int fd = open("/dev/urandom", O_RDONLY);
    unsigned char raw[8] = {0};
    if (fd >= 0) { read(fd, raw, 8); close(fd); }
    else { 
        unsigned long t = (unsigned long)time(NULL);
        for (int i=0;i<8;i++) { raw[i]=(t>>(i*8))&0xff; }
    }
    lg_hex(raw, 8, out);
}

static int lg_mount_persist(void) {
    
    const char *devices[] = {
        "/dev/sda2","/dev/sda3",
        "/dev/sdb1","/dev/sdb2","/dev/sdb3",
        "/dev/sdc1","/dev/sdc2","/dev/sdc3",
        "/dev/vda2","/dev/vda3",
        NULL
    };
    mkdir(PERSIST_MOUNT, 0755);

    if (mount("LABEL=TRIUMPH_DATA", PERSIST_MOUNT, "ext4", 0, "") == 0) return 1;
    if (mount("LABEL=TRIUMPH_DATA", PERSIST_MOUNT, "vfat", 0, "") == 0) return 1;

    for (int i=0; devices[i]; i++) {
        if (mount(devices[i], PERSIST_MOUNT, "ext4", 0, "") == 0) return 1;
        if (mount(devices[i], PERSIST_MOUNT, "vfat", 0, "") == 0) return 1;
    }
    return 0;
}

static void lg_load_users(void) {
    g_nusers = 0;
    int fd = open(USERS_DB, O_RDONLY);
    if (fd < 0) return;
    char buf[4096] = {0};
    int n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return;

    char *p = buf;
    while (*p && g_nusers < MAX_USERS) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        
        char *c1 = strchr(p, ':');
        if (!c1) { p = eol ? eol+1 : p+strlen(p); continue; }
        *c1 = 0;
        char *c2 = strchr(c1+1, ':');
        if (!c2) { p = eol ? eol+1 : p+strlen(p); continue; }
        *c2 = 0;
        strncpy(g_users[g_nusers].name, p,    MAX_NAME-1);
        strncpy(g_users[g_nusers].salt, c1+1, MAX_SALT-1);
        strncpy(g_users[g_nusers].hash, c2+1, MAX_HASH-1);
        g_nusers++;
        p = eol ? eol+1 : p+strlen(p);
    }
}

static void lg_save_users(void) {
    if (!g_persist_ok) return;
    mkdir(HOMES_DIR, 0755);
    int fd = open(USERS_DB, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    if (fd < 0) return;
    for (int i=0; i<g_nusers; i++) {
        char line[256];
        snprintf(line, sizeof(line), "%s:%s:%s\n",
                 g_users[i].name, g_users[i].salt, g_users[i].hash);
        write(fd, line, strlen(line));
    }
    close(fd);
}

static int lg_user_exists(const char *name) {
    for (int i=0; i<g_nusers; i++)
        if (strcmp(g_users[i].name, name)==0) return i;
    return -1;
}

static int lg_check_password(int idx, const char *pass) {
    char hash[MAX_HASH];
    lg_hash(g_users[idx].salt, pass, hash);
    return strcmp(hash, g_users[idx].hash)==0;
}

static void lg_add_user(const char *name, const char *pass) {
    if (g_nusers >= MAX_USERS) return;
    strncpy(g_users[g_nusers].name, name, MAX_NAME-1);
    lg_make_salt(g_users[g_nusers].salt);
    lg_hash(g_users[g_nusers].salt, pass, g_users[g_nusers].hash);
    g_nusers++;
    lg_save_users();
    
    char home[256];
    snprintf(home, sizeof(home), "%s/%s", HOMES_DIR, name);
    mkdir(home, 0700);
}

static void lg_paint_bg(int rows, int cols) {
    lg_w(LG_BG);
    lg_at(1,1);
    for (int r=0;r<rows;r++) {
        for (int c=0;c<cols;c++) lg_w(" ");
        if (r<rows-1) lg_w("\r\n");
    }
}

static const char *LG_BANNER[] = {
    "  ████████╗██████╗ ██╗██╗   ██╗███╗   ███╗██████╗ ██╗  ██╗",
    "  ╚══██╔══╝██╔══██╗██║██║   ██║████╗ ████║██╔══██╗██║  ██║",
    "     ██║   ██████╔╝██║██║   ██║██╔████╔██║██████╔╝███████║",
    "     ██║   ██╔══██╗██║██║   ██║██║╚██╔╝██║██╔═══╝ ██╔══██║",
    "     ██║   ██║  ██║██║╚██████╔╝██║ ╚═╝ ██║██║     ██║  ██║",
    "     ╚═╝   ╚═╝  ╚═╝╚═╝ ╚═════╝ ╚═╝     ╚═╝╚═╝     ╚═╝  ╚═╝",
    NULL
};

static void lg_read_field(const char *prompt, char *buf, int bufsz,
                           int echo, int row, int col, int fieldw) {
    struct termios old, raw;
    tcgetattr(0, &old); raw=old;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    tcsetattr(0, TCSANOW, &raw);

    lg_at(row, col);
    lg_w(LG_YEL"\x1b[1m"); lg_w(prompt); lg_w("\x1b[22m");
    lg_w(LG_FG"┌");
    for(int i=0;i<fieldw;i++) lg_w("─");
    lg_w("┐");
    lg_at(row+1, col);
    lg_w("│ ");
    for(int i=0;i<fieldw-1;i++) lg_w(" ");
    lg_w("│");
    lg_at(row+2, col);
    lg_w("└");
    for(int i=0;i<fieldw;i++) lg_w("─");
    lg_w("┘\x1b[?25h");

    int len = 0;
    buf[0] = 0;
    while (1) {
        lg_at(row+1, col+2);
        if (echo) {
            char pad[128]; snprintf(pad, sizeof(pad), "%-*s", fieldw-2, buf);
            lg_w(LG_YEL); lg_w(pad);
        } else {
            
            char stars[128] = {0};
            for (int i=0;i<len && i<fieldw-3;i++) stars[i]='*';
            char pad[128]; snprintf(pad,sizeof(pad),"%-*s",fieldw-2,stars);
            lg_w(LG_YEL); lg_w(pad);
        }
        lg_at(row+1, col+2+len);
        fflush(stdout);

        unsigned char c;
        read(0, &c, 1);
        if (c==13||c==10) break;
        if ((c==127||c==8) && len>0) { len--; buf[len]=0; }
        else if (c>=32 && c<127 && len<bufsz-1) { buf[len++]=(char)c; buf[len]=0; }
    }
    lg_w("\x1b[?25l");
    tcsetattr(0, TCSANOW, &old);
}

static void lg_message(int row, int col, const char *msg, const char *color) {
    lg_at(row, col);
    lg_w(color); lg_w(msg);
    fflush(stdout);
    sleep(1);
    
    lg_at(row, col);
    char blank[80]; snprintf(blank, sizeof(blank), "%-*s", (int)strlen(msg), "");
    lg_w(blank);
}

static void lg_draw_screen(int rows, int cols, const char *subtitle) {
    lg_paint_bg(rows, cols);
    int bx = (cols - 58) / 2; if (bx<1) bx=1;
    for (int i=0; LG_BANNER[i]; i++) {
        lg_at(2+i, bx);
        lg_w(LG_FG); lg_w(LG_BANNER[i]);
    }
    if (subtitle) {
        int sx = (cols - (int)strlen(subtitle)) / 2;
        lg_at(9, sx);
        lg_w(LG_YEL"\x1b[1m"); lg_w(subtitle); lg_w("\x1b[22m");
    }
    lg_at(10, 1); lg_w(LG_DIM);
    for (int c=0;c<cols;c++) lg_w("═");
}

static int lg_readkey_nb(void) {
    struct termios cur; tcgetattr(0,&cur);
    struct termios tmp=cur; tmp.c_cc[VMIN]=0; tmp.c_cc[VTIME]=0;
    tcsetattr(0,TCSANOW,&tmp);
    unsigned char c; int r=read(0,&c,1);
    tcsetattr(0,TCSANOW,&cur);
    return r>0?(int)c:-1;
}

static int lg_do_login(int rows, int cols) {
    char uname[MAX_NAME], pass[128], msg[128];
    int cx = (cols-40)/2; if(cx<4) cx=4;

    lg_draw_screen(rows, cols, "── LOGIN ──");
    lg_read_field("Username:", uname, sizeof(uname), 1, 12, cx, 36);

    int idx = lg_user_exists(uname);
    if (idx < 0) {
        lg_message(17, cx, "User not found.", LG_RED);
        return 0;
    }

    lg_draw_screen(rows, cols, "── LOGIN ──");
    lg_at(12, cx); lg_w(LG_DIM2"Username: "LG_FG); lg_w(uname);
    lg_read_field("Password:", pass, sizeof(pass), 0, 14, cx, 36);

    if (!lg_check_password(idx, pass)) {
        lg_message(19, cx, "Wrong password.", LG_RED);
        return 0;
    }

    strncpy(g_current_user, uname, MAX_NAME-1);

    char home[256];
    snprintf(home, sizeof(home), "%s/%s", HOMES_DIR, uname);
    mkdir(home, 0700);
    setenv("HOME", home, 1);
    setenv("USER", uname, 1);
    chdir(home);

    snprintf(msg, sizeof(msg), "Welcome back, %s!", uname);
    lg_draw_screen(rows, cols, NULL);
    int mx = (cols-(int)strlen(msg))/2;
    lg_at(rows/2, mx);
    lg_w(LG_GRN"\x1b[1m"); lg_w(msg); lg_w("\x1b[22m");
    fflush(stdout);
    sleep(1);
    return 1;
}

static void lg_do_register(int rows, int cols) {
    char uname[MAX_NAME], pass[128], pass2[128];
    int cx = (cols-40)/2; if(cx<4) cx=4;

    lg_draw_screen(rows, cols, "── CREATE ACCOUNT ──");
    lg_read_field("Choose username:", uname, sizeof(uname), 1, 12, cx, 36);

    if (strlen(uname) < 2) { lg_message(17,cx,"Username too short (min 2).",LG_RED); return; }
    if (lg_user_exists(uname) >= 0) { lg_message(17,cx,"Username already taken.",LG_RED); return; }
    
    for (int i=0; uname[i]; i++) {
        if (!isalnum((unsigned char)uname[i]) && uname[i]!='_') {
            lg_message(17,cx,"Letters/numbers/underscore only.",LG_RED); return;
        }
    }

    lg_draw_screen(rows, cols, "── CREATE ACCOUNT ──");
    lg_at(12, cx); lg_w(LG_DIM2"Username: "LG_FG); lg_w(uname);
    lg_read_field("Choose password:", pass, sizeof(pass), 0, 14, cx, 36);

    if (strlen(pass) < 4) { lg_message(19,cx,"Password too short (min 4).",LG_RED); return; }

    lg_draw_screen(rows, cols, "── CREATE ACCOUNT ──");
    lg_at(12, cx); lg_w(LG_DIM2"Username: "LG_FG); lg_w(uname);
    lg_at(13, cx); lg_w(LG_DIM2"Password: "LG_FG); lg_w("****");
    lg_read_field("Confirm password:", pass2, sizeof(pass2), 0, 15, cx, 36);

    if (strcmp(pass, pass2)!=0) { lg_message(20,cx,"Passwords don't match.",LG_RED); return; }

    if (!g_persist_ok) {
        lg_message(20,cx,"No persist partition — account won't survive reboot.",LG_YEL);
        sleep(2);
    }

    lg_add_user(uname, pass);

    char msg[64]; snprintf(msg, sizeof(msg), "Account '%s' created!", uname);
    lg_draw_screen(rows, cols, NULL);
    int mx=(cols-(int)strlen(msg))/2;
    lg_at(rows/2, mx);
    lg_w(LG_GRN"\x1b[1m"); lg_w(msg); lg_w("\x1b[22m");
    fflush(stdout);
    sleep(2);
}

static void lg_run(void) {
    struct termios old, raw;
    tcgetattr(0,&old); raw=old;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    tcsetattr(0,TCSANOW,&raw);

    struct winsize ws; ioctl(0,TIOCGWINSZ,&ws);
    int rows=ws.ws_row?ws.ws_row:24;
    int cols=ws.ws_col?ws.ws_col:80;

    lg_w("\x1b[?25l\x1b[2J");

    g_persist_ok = lg_mount_persist();
    if (g_persist_ok) {
        mkdir(HOMES_DIR, 0755);
        lg_load_users();
    }

    if (g_nusers == 0) {
        
        lg_draw_screen(rows, cols, "── FIRST BOOT ──");
        int cx = (cols-50)/2; if(cx<2) cx=2;
        lg_at(12, cx); lg_w(LG_FG"No accounts exist yet. Create your first account.");
        if (!g_persist_ok) {
            lg_at(13, cx); lg_w(LG_YEL"Warning: no persist partition found.");
            lg_at(14, cx); lg_w(LG_DIM2"Accounts will be lost on reboot.");
            lg_at(15, cx); lg_w(LG_DIM2"Run setup-persist from shell to set up USB storage.");
        }
        fflush(stdout);
        sleep(2);
        lg_do_register(rows, cols);
        
        if (g_nusers > 0) {
            strncpy(g_current_user, g_users[0].name, MAX_NAME-1);
            char home[256];
            snprintf(home, sizeof(home), "%s/%s", HOMES_DIR, g_users[0].name);
            mkdir(home, 0700);
            setenv("HOME", home, 1);
            setenv("USER", g_users[0].name, 1);
            chdir(home);
            goto done;
        }
    }

    while (1) {
        lg_draw_screen(rows, cols, NULL);
        int cx = (cols-40)/2; if(cx<4) cx=4;

        lg_at(12, cx); lg_w(LG_FG"\x1b[1mL\x1b[22m"LG_DIM2"  Login");
        lg_at(13, cx); lg_w(LG_FG"\x1b[1mN\x1b[22m"LG_DIM2"  New account");
        lg_at(14, cx); lg_w(LG_FG"\x1b[1mG\x1b[22m"LG_DIM2"  Continue as guest (no persistence)");

        if (!g_persist_ok) {
            lg_at(rows-3, 2);
            lg_w(LG_YEL"No persist partition — accounts won't survive reboot.");
        }
        fflush(stdout);

        unsigned char c; read(0,&c,1);
        if (c=='l'||c=='L') {
            if (lg_do_login(rows, cols)) goto done;
        } else if (c=='n'||c=='N') {
            lg_do_register(rows, cols);
        } else if (c=='g'||c=='G') {
            strcpy(g_current_user, "guest");
            setenv("USER", "guest", 1);
            setenv("HOME", "/root", 1);
            chdir("/root");
            goto done;
        }
    }

done:
    lg_w("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    tcsetattr(0,TCSANOW,&old);
}

static void login_init(void) {
    lg_run();
    
    setenv("USER", g_current_user, 1);
}

static int b_logout(Cmd *c) { (void)c;
    printf("\x1b[1m\x1b[38;5;226mLogging out %s...\x1b[0m\n", g_current_user);
    fflush(stdout);
    sleep(1);
    
    strcpy(g_current_user, "");
    setenv("HOME", "/root", 1);
    setenv("USER", "root", 1);
    chdir("/root");
    
    login_init();
    return 0;
}
