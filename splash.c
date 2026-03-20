/*
 * splash.c — Triumph OS boot splash animation
 * Called from init.c before exec'ing the shell.
 * Included with: #include "splash.c"
 */

static void splash_show(void) {
    /* Only show if stdout is a real TTY */
    if (!isatty(STDOUT_FILENO)) return;

    const char *frames[] = {
        "\x1b[38;5;17m",
        "\x1b[38;5;18m",
        "\x1b[38;5;19m",
        "\x1b[38;5;20m",
        "\x1b[38;5;21m",
        "\x1b[38;5;27m",
        "\x1b[38;5;33m",
        "\x1b[38;5;39m",
        "\x1b[38;5;45m",
        "\x1b[38;5;51m",
    };

    /* hide cursor, clear screen */
    write(STDOUT_FILENO, "\x1b[?25l\x1b[2J", 11);

    /* get terminal size */
    struct winsize ws;
    int cols = 80, rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)==0 && ws.ws_col && ws.ws_row)
        { cols=ws.ws_col; rows=ws.ws_row; }

    /* centre the logo */
    int logo_w = 58;
    int logo_h = 13;
    int ox = (cols - logo_w) / 2; if (ox < 1) ox = 1;
    int oy = (rows - logo_h) / 2; if (oy < 2) oy = 2;

    const char *logo[] = {
        "  ████████╗██████╗ ██╗██╗   ██╗███╗   ███╗██████╗ ██╗  ██╗",
        "  ╚══██╔══╝██╔══██╗██║██║   ██║████╗ ████║██╔══██╗██║  ██║",
        "     ██║   ██████╔╝██║██║   ██║██╔████╔██║██████╔╝███████║",
        "     ██║   ██╔══██╗██║██║   ██║██║╚██╔╝██║██╔═══╝ ██╔══██║",
        "     ██║   ██║  ██║██║╚██████╔╝██║ ╚═╝ ██║██║     ██║  ██║",
        "     ╚═╝   ╚═╝  ╚═╝╚═╝ ╚═════╝ ╚═╝     ╚═╝╚═╝     ╚═╝  ╚═╝",
        "",
        "                    ██████╗ ███████╗",
        "                   ██╔═══██╗██╔════╝",
        "                   ██║   ██║███████╗",
        "                   ██║   ██║╚════██║",
        "                   ╚██████╔╝███████║",
        "                    ╚═════╝ ╚══════╝",
    };

    /* animate: fade in colour frame by frame */
    int nframes = 10;
    for (int f = 0; f < nframes; f++) {
        /* position and draw logo in current colour */
        for (int l = 0; l < logo_h; l++) {
            /* move cursor */
            char mv[32]; snprintf(mv, sizeof(mv), "\x1b[%d;%dH", oy + l, ox);
            write(STDOUT_FILENO, mv, strlen(mv));
            write(STDOUT_FILENO, frames[f], strlen(frames[f]));
            write(STDOUT_FILENO, "\x1b[1m", 4);
            write(STDOUT_FILENO, logo[l], strlen(logo[l]));
            write(STDOUT_FILENO, "\x1b[0m", 4);
        }

        /* tagline fades in on last few frames */
        if (f >= 7) {
            char tag[128];
            const char *tagline = "TTY-only live OS  —  boot from USB, install nothing";
            int tx = (cols - (int)strlen(tagline)) / 2;
            snprintf(tag, sizeof(tag), "\x1b[%d;%dH\x1b[38;5;240m%s\x1b[0m",
                     oy + logo_h + 1, tx, tagline);
            write(STDOUT_FILENO, tag, strlen(tag));
        }

        usleep(60000);  /* 60ms per frame = ~600ms total */
    }

    /* hold for a moment then clear */
    usleep(800000);
    write(STDOUT_FILENO, "\x1b[2J\x1b[H\x1b[?25h", 15);
}
