/*
 * init.c — Triumph OS PID 1
 * Robust TTY setup before handing off to the shell.
 */
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
    /* mount virtual fs */
    do_mount("proc",    "/proc","proc",   MS_NODEV|MS_NOSUID|MS_NOEXEC);
    do_mount("sysfs",   "/sys", "sysfs",  MS_NODEV|MS_NOSUID|MS_NOEXEC);
    do_mount("devtmpfs","/dev", "devtmpfs",MS_NOSUID|MS_STRICTATIME);
    do_mount("tmpfs",   "/tmp", "tmpfs",  MS_NODEV|MS_NOSUID);
    do_mount("tmpfs",   "/run", "tmpfs",  MS_NODEV|MS_NOSUID);

    /* guarantee essential device nodes exist */
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

    /* silence kernel messages on the console */
    { int fd=open("/proc/sys/kernel/printk",O_WRONLY); if(fd>=0){write(fd,"1 4 1 7",7);close(fd);} }

    mkdir("/root",0700);
    chdir("/root");

    kmsg("starting triumph shell");

    /* spawn + respawn loop */
    while(1){
        pid_t pid=fork();
        if(pid==0){
            /* child: open TTY, set env, exec shell */
            open_tty();
            setup_env();
            char *argv[]={"triumph",NULL};
            execv("/bin/triumph",argv);
            /* exec failed — write error visibly */
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
