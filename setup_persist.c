static int b_setup_persist(Cmd *c) { (void)c;
    printf(BLD CYN "Triumph OS — Persistent Storage Setup\n" RST);
    printf(GRY "This will create a writable partition on your USB drive\n");
    printf("for saving accounts, files, and settings across reboots.\n\n" RST);

    char dev[64] = "";

    FILE *f = fopen("/proc/mounts", "r");
    if (f) {
        char line[256], mdev[64], mpoint[64], mfs[32];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%63s %63s %31s", mdev, mpoint, mfs) == 3) {
                if (strcmp(mpoint,"/") == 0 && strncmp(mdev,"/dev/sd",7)==0) {
                    strncpy(dev, mdev, sizeof(dev)-1);
                    
                    int l = strlen(dev);
                    while (l > 0 && isdigit((unsigned char)dev[l-1])) { dev[l-1]=0; l--; }
                    break;
                }
            }
        }
        fclose(f);
    }

    if (!dev[0]) {
        
        const char *tries[] = {"/dev/sda","/dev/sdb","/dev/sdc",NULL};
        printf(GRY "Auto-detect failed. Available block devices:\n" RST);
        for (int i=0; tries[i]; i++) {
            char sz_path[64]; snprintf(sz_path,sizeof(sz_path),"/sys/block/%s/size",
                                       tries[i]+5);
            int fd=open(sz_path,O_RDONLY);
            if (fd>=0) {
                char buf[32]={0}; read(fd,buf,sizeof(buf)-1); close(fd);
                long sectors = atol(buf);
                printf("  %s  (%ld MB)\n", tries[i], sectors*512/1024/1024);
            }
        }
        printf(YLW "\nEnter device (e.g. /dev/sdb): " RST);
        fflush(stdout);
        if (!fgets(dev, sizeof(dev), stdin)) return 1;
        dev[strcspn(dev,"\n")] = 0;
    }

    if (!dev[0]) { printf(RED "No device selected.\n" RST); return 1; }

    printf(YLW "\nDevice: " BLD "%s\n" RST, dev);
    printf(GRY "This will create a new ext4 partition on %s.\n", dev);
    printf("Existing data on the USB will NOT be affected.\n\n" RST);
    printf(YLW "Type YES to continue: " RST); fflush(stdout);

    char confirm[16]={0};
    if (!fgets(confirm, sizeof(confirm), stdin)) return 1;
    confirm[strcspn(confirm,"\n")] = 0;
    if (strcmp(confirm,"YES") != 0) {
        printf(GRY "Cancelled.\n" RST); return 0;
    }

    printf(CYN "Creating partition...\n" RST); fflush(stdout);

    char cmd[256];
    
    snprintf(cmd, sizeof(cmd),
        "echo ',,L' | sfdisk --append %s 2>&1", dev);
    FILE *p = popen(cmd, "r");
    if (p) {
        char buf[256];
        while (fgets(buf, sizeof(buf), p)) printf(GRY "%s" RST, buf);
        pclose(p);
    }

    char newpart[64] = "";
    for (int n=1; n<=9; n++) {
        snprintf(newpart, sizeof(newpart), "%s%d", dev, n);
        int fd=open(newpart, O_RDONLY);
        if (fd>=0) {
            close(fd);
            
            int mounted=0;
            FILE *mf=fopen("/proc/mounts","r");
            if (mf) {
                char line[256], mp[64], ms[64], mfs[32];
                while (fgets(line,sizeof(line),mf)) {
                    if (sscanf(line,"%63s %63s %31s",mp,ms,mfs)==3 &&
                        strcmp(mp,newpart)==0) { mounted=1; break; }
                }
                fclose(mf);
            }
            if (!mounted) strcpy(newpart, newpart); 
        }
    }

    printf(CYN "Formatting %s as ext4...\n" RST, newpart); fflush(stdout);
    snprintf(cmd, sizeof(cmd), "mkfs.ext4 -L TRIUMPH_DATA %s 2>&1", newpart);
    p = popen(cmd, "r");
    if (p) {
        char buf[256];
        while (fgets(buf, sizeof(buf), p)) printf(GRY "%s" RST, buf);
        pclose(p);
    }

    mkdir("/persist", 0755);
    if (mount(newpart, "/persist", "ext4", 0, "") == 0) {
        mkdir("/persist/home", 0755);
        /* persist mounted */
        printf(GRN BLD "\nDone! Persistent storage is now active.\n" RST);
        printf(GRY "Your accounts and files will survive reboots.\n" RST);
        printf(YLW "Restart Triumph to use the new login system: " RST);
        printf(BLD "reboot\n" RST);
    } else {
        printf(RED "Mount failed: %s\n" RST, strerror(errno));
        printf(GRY "Try running: mount %s /persist\n" RST, newpart);
    }

    return 0;
}
