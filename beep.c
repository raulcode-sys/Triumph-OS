

#include <linux/kd.h>

#define PC_CLOCK 1193180

static int pc_fd = -1;

static int pc_open(void) {
    if (pc_fd >= 0) return pc_fd;
    pc_fd = open("/dev/console", O_WRONLY);
    if (pc_fd < 0) pc_fd = open("/dev/tty0", O_WRONLY);
    return pc_fd;
}

static void pc_tone(int hz, int ms) {
    int fd = pc_open();
    if (fd < 0) { usleep(ms * 1000); return; }
    if (hz <= 0) {
        ioctl(fd, KIOCSOUND, 0);
        usleep(ms * 1000);
        return;
    }
    
    int div = PC_CLOCK / hz;
    if (ioctl(fd, KIOCSOUND, div) < 0) {
        
        usleep(ms * 1000);
        return;
    }
    usleep(ms * 1000);
    ioctl(fd, KIOCSOUND, 0);
}

typedef struct { int hz; int ms; } PcNote;

static void pc_play(const PcNote *seq) {
    for (int i = 0; seq[i].hz || seq[i].ms; i++) {
        pc_tone(seq[i].hz, seq[i].ms);
    }
}

static const PcNote SND_SHUTDOWN[] = {
    { 880, 100 },
    { 660, 100 },
    { 440, 200 },
    { 0, 0 }
};

static const PcNote SND_BOOT[] = {
    { 880, 120 },
    { 0,    80 },
    { 880, 120 },
    { 0, 0 }
};

static const PcNote SND_JUMP[] = {
    { 600, 30 },
    { 800, 30 },
    { 1100, 40 },
    { 0, 0 }
};

static const PcNote SND_BLIP[] = {
    { 880, 30 },
    { 0, 0 }
};

static const PcNote SND_BLIP_LO[] = {
    { 440, 30 },
    { 0, 0 }
};

static const PcNote SND_SCORE[] = {
    { 440, 80 },
    { 330, 100 },
    { 0, 0 }
};

static const PcNote SND_THUNK[] = {
    { 200, 40 },
    { 100, 60 },
    { 0, 0 }
};

static const PcNote SND_LINE[] = {
    { 880, 50 },
    { 1046, 50 },
    { 1318, 80 },
    { 0, 0 }
};

static const PcNote SND_NOM[] = {
    { 1320, 60 },
    { 0, 0 }
};

static const PcNote SND_GAMEOVER[] = {
    { 440, 200 },
    { 392, 200 },
    { 349, 200 },
    { 294, 400 },
    { 0, 0 }
};
