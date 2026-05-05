#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include "splash.c"
#include "audio.c"


/* Load a kernel module via the init_module syscall */
#include <sys/syscall.h>
static int load_module(const char *path, char *errbuf, size_t errsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errbuf) snprintf(errbuf, errsz, "open: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); if(errbuf)snprintf(errbuf,errsz,"stat fail"); return -1; }
    void *data = malloc(st.st_size);
    if (!data) { close(fd); if(errbuf)snprintf(errbuf,errsz,"malloc fail"); return -1; }
    if (read(fd, data, st.st_size) != st.st_size) {
        close(fd); free(data);
        if(errbuf)snprintf(errbuf,errsz,"read fail");
        return -1;
    }
    close(fd);
    long rc = syscall(SYS_init_module, data, (unsigned long)st.st_size, "");
    if (rc < 0 && errno != EEXIST) {
        if (errbuf) snprintf(errbuf, errsz, "init_module: %s (errno=%d)", strerror(errno), errno);
        free(data);
        return -1;
    }
    free(data);
    return 0;
}

static void kmsg(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd < 0) fd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd >= 0) { write(fd,"triumph: ",9); write(fd,msg,strlen(msg)); write(fd,"\n",1); close(fd); }
}

static void do_mount(const char *src,const char *tgt,const char *type,unsigned long flags){
    mount(src,tgt,type,flags,NULL);
}

static void make_dev(const char *path,mode_t mode,int maj,int min){
    unlink(path);
    mknod(path,mode,makedev(maj,min));
    chmod(path,mode&0777);
}

static void open_tty(void) {
    const char *ttys[]={"/dev/tty1","/dev/tty0","/dev/console","/dev/tty",NULL};
    setsid();
    for(int i=0;ttys[i];i++){
        int fd=open(ttys[i],O_RDWR|O_NOCTTY);
        if(fd<0) continue;
        ioctl(fd,TIOCSCTTY,1);
        close(0);close(1);close(2);
        dup2(fd,0);dup2(fd,1);dup2(fd,2);
        if(fd>2)close(fd);
        return;
    }
}

static void setup_env(void){
    clearenv();
    setenv("PATH",  "/bin:/sbin:/usr/bin:/usr/sbin",1);
    setenv("HOME",  "/root",1);
    setenv("TERM",  "linux",1);
    setenv("USER",  "root",1);
    setenv("SHELL", "/bin/triumph",1);
    setenv("LOGNAME","root",1);
}

int main(void){
    do_mount("proc",    "/proc","proc",   MS_NODEV|MS_NOSUID|MS_NOEXEC);
    do_mount("sysfs",   "/sys", "sysfs",  MS_NODEV|MS_NOSUID|MS_NOEXEC);
    do_mount("devtmpfs","/dev", "devtmpfs",MS_NOSUID|MS_STRICTATIME);
    do_mount("tmpfs",   "/tmp", "tmpfs",  MS_NODEV|MS_NOSUID);
    do_mount("tmpfs",   "/run", "tmpfs",  MS_NODEV|MS_NOSUID);

    make_dev("/dev/null",   S_IFCHR|0666,1,3);
    make_dev("/dev/zero",   S_IFCHR|0666,1,5);
    make_dev("/dev/urandom",S_IFCHR|0666,1,9);
    make_dev("/dev/kmsg",   S_IFCHR|0600,1,11);
    make_dev("/dev/console",S_IFCHR|0600,5,1);
    make_dev("/dev/tty",    S_IFCHR|0666,5,0);
    make_dev("/dev/tty0",   S_IFCHR|0620,4,0);
    make_dev("/dev/tty1",   S_IFCHR|0620,4,1);
    make_dev("/dev/tty2",   S_IFCHR|0620,4,2);
    make_dev("/dev/ttyS0",  S_IFCHR|0660,4,64);

    unlink("/dev/stdin");  symlink("/proc/self/fd/0","/dev/stdin");
    unlink("/dev/stdout"); symlink("/proc/self/fd/1","/dev/stdout");
    unlink("/dev/stderr"); symlink("/proc/self/fd/2","/dev/stderr");

    { int fd=open("/proc/sys/kernel/printk",O_WRONLY); if(fd>=0){write(fd,"1 4 1 7",7);close(fd);} }

    mkdir("/root",0700);
    chdir("/root");

    open_tty();

    /* Load Realtek PHY driver first (soft dep of r8169) */
    {
        char err[200] = "";
        char p1[] = "/lib/modules/6.8.0-111-generic/kernel/drivers/net/phy/realtek.ko";
        if (load_module(p1, err, sizeof(err)) == 0) kmsg("loaded realtek PHY");
        else { char b[256]; snprintf(b,sizeof(b),"realtek PHY load failed: %s",err); kmsg(b); }
    }

    /* Load Realtek r8169 ethernet driver — works for ThinkPad E470 and others */
    {
        char path[] = "/lib/modules/6.8.0-111-generic/kernel/drivers/net/ethernet/realtek/r8169.ko";
        char err[200] = "";
        if (load_module(path, err, sizeof(err)) == 0) {
            kmsg("loaded r8169 ethernet driver");
        } else {
            char buf[256]; snprintf(buf, sizeof(buf), "r8169 load failed: %s", err);
            kmsg(buf);
            int fd = open("/tmp/r8169_error.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if (fd >= 0) { write(fd, buf, strlen(buf)); write(fd, "\n", 1); close(fd); }
        }
    }

    /* Load HDA audio stack — order matters */
    {
        const char *audio_modules[] = {
            "/lib/modules/6.8.0-111-generic/kernel/sound/soundcore.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/core/snd.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/core/snd-timer.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/core/snd-pcm.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/core/snd-hwdep.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/hda/snd-intel-sdw-acpi.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/hda/snd-intel-dspcfg.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/hda/snd-hda-core.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/pci/hda/snd-hda-codec.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/pci/hda/snd-hda-codec-generic.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/pci/hda/snd-hda-codec-realtek.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/pci/hda/snd-hda-codec-hdmi.ko",
            "/lib/modules/6.8.0-111-generic/kernel/sound/pci/hda/snd-hda-intel.ko",
            NULL
        };
        char audio_log[1024] = "";
        for (int i = 0; audio_modules[i]; i++) {
            char err[200] = "";
            if (load_module(audio_modules[i], err, sizeof(err)) == 0) {
                strncat(audio_log, "OK: ", sizeof(audio_log)-strlen(audio_log)-1);
            } else {
                strncat(audio_log, "FAIL: ", sizeof(audio_log)-strlen(audio_log)-1);
                strncat(audio_log, err,    sizeof(audio_log)-strlen(audio_log)-1);
                strncat(audio_log, " ",    sizeof(audio_log)-strlen(audio_log)-1);
            }
            const char *base = strrchr(audio_modules[i], '/');
            strncat(audio_log, base ? base+1 : audio_modules[i],
                    sizeof(audio_log)-strlen(audio_log)-1);
            strncat(audio_log, "\n", sizeof(audio_log)-strlen(audio_log)-1);
        }
        int afd = open("/tmp/audio_log.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (afd >= 0) { write(afd, audio_log, strlen(audio_log)); close(afd); }
        kmsg("audio modules loaded");
    }

    /* Give kernel a moment to bind audio devices and create /dev/snd entries */
    sleep(2);

    /* Start boot WAV in background — plays during the splash */
    audio_play_wav_async("/boot.wav");

    splash_show();

    kmsg("starting triumph shell");

    while(1){
        pid_t pid=fork();
        if(pid==0){
            open_tty();
            setup_env();
            char *argv[]={"triumph",NULL};
            execv("/bin/triumph",argv);
            write(2,"triumph-init: exec failed!\r\n",28);
            execv("/bin/sh",argv);
            _exit(1);
        }
        if(pid<0){ sleep(2); continue; }
        int status;
        waitpid(pid,&status,0);
        kmsg("shell exited, respawning");
        sleep(1);
    }
    return 0;
}
