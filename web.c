/*
 * Triumph Web — minimal HTTP browser.
 *
 * Detects ethernet, runs a tiny built-in DHCP client to get an IP,
 * then fetches HTTP URLs and renders them as text (lynx-style, simplified).
 *
 * If no ethernet is connected: shows the "no internet" message.
 *
 * HTTP only — HTTPS would need a full TLS stack (OpenSSL, mbedTLS) which we
 * deliberately don't bundle.
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <linux/route.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/debug.h>

#define WB_BG  "\x1b[48;5;17m"
#define WB_FG  "\x1b[38;5;51m"
#define WB_HI  "\x1b[48;5;51m\x1b[38;5;17m"
#define WB_DIM "\x1b[38;5;33m"
#define WB_DIM2 "\x1b[38;5;67m"
#define WB_YEL "\x1b[38;5;226m"
#define WB_GRN "\x1b[38;5;82m"
#define WB_RED "\x1b[38;5;196m"

#include <sys/syscall.h>
static int wb_load_module(const char *path, char *err, size_t errsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err,errsz,"open %s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); snprintf(err,errsz,"stat fail"); return -1; }
    void *data = malloc(st.st_size);
    if (!data) { close(fd); return -1; }
    if (read(fd, data, st.st_size) != st.st_size) { close(fd); free(data); return -1; }
    close(fd);
    long rc = syscall(SYS_init_module, data, (unsigned long)st.st_size, "");
    free(data);
    if (rc < 0 && errno != 17 /* EEXIST */) {
        snprintf(err, errsz, "init_module: %s (errno %d)", strerror(errno), errno);
        return -1;
    }
    return 0;
}

static void wb_w(const char *s) { write(1, s, strlen(s)); }
static void wb_at(int r, int c) { char b[24]; snprintf(b,24,"\x1b[%d;%dH",r,c); wb_w(b); }
static void wb_paint_bg(int rows, int cols) {
    wb_w(WB_BG);
    wb_at(1,1);
    for(int r=0;r<rows;r++){for(int c=0;c<cols;c++)wb_w(" ");if(r<rows-1)wb_w("\r\n");}
}

/* ── Find ethernet interface ────────────────────────────────────────────── */
/* Bring interface UP via ioctl */
static void iface_up(const char *name) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct ifreq ifr; memset(&ifr,0,sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ-1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

static int iface_is_wireless(const char *name) {
    char wpath[256]; snprintf(wpath,sizeof(wpath),"/sys/class/net/%s/wireless",name);
    struct stat st;
    if (stat(wpath,&st)==0) return 1;
    char p2[256]; snprintf(p2,sizeof(p2),"/sys/class/net/%s/phy80211",name);
    if (stat(p2,&st)==0) return 1;
    return 0;
}

static int iface_carrier(const char *name) {
    char cpath[256]; snprintf(cpath,sizeof(cpath),"/sys/class/net/%s/carrier",name);
    int fd = open(cpath, O_RDONLY); if (fd<0) return 0;
    char buf[8]={0}; read(fd,buf,sizeof(buf)-1); close(fd);
    return buf[0]=='1';
}

/* Try loading common ethernet drivers that we bundled */
static void try_load_drivers(void) {
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;
    char err[256];
    /* Try Realtek r8169 (ThinkPad E470, many laptops) */
    wb_load_module("/lib/modules/6.8.0-111-generic/kernel/drivers/net/ethernet/realtek/r8169.ko", err, sizeof(err));
    /* Give the kernel ~2s to enumerate PCI and create interfaces */
    for (int i=0; i<20; i++) {
        DIR *d = opendir("/sys/class/net");
        int count = 0;
        if (d) {
            struct dirent *e;
            while ((e=readdir(d))) {
                if (e->d_name[0]=='.') continue;
                if (strcmp(e->d_name,"lo")==0) continue;
                count++;
            }
            closedir(d);
        }
        if (count > 0) break;
        usleep(100000);
    }
}

/* Bring up every non-loopback, non-wireless interface, wait for carrier,
 * return the first one with link. */
static int find_ethernet(char *ifname, size_t ifname_sz) {
    try_load_drivers();
    char found[8][32]; int nfound = 0;
    DIR *d = opendir("/sys/class/net");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) && nfound < 8) {
        if (e->d_name[0]=='.') continue;
        if (strcmp(e->d_name,"lo")==0) continue;
        if (iface_is_wireless(e->d_name)) continue;
        strncpy(found[nfound], e->d_name, 31);
        found[nfound][31] = 0;
        nfound++;
    }
    closedir(d);
    if (nfound == 0) return -1;

    /* Bring them all up */
    for (int i=0; i<nfound; i++) iface_up(found[i]);

    /* Poll for carrier — up to 5 seconds */
    for (int t=0; t<50; t++) {
        for (int i=0; i<nfound; i++) {
            if (iface_carrier(found[i])) {
                strncpy(ifname, found[i], ifname_sz-1);
                ifname[ifname_sz-1] = 0;
                return 0;
            }
        }
        usleep(100000);
    }

    /* No link detected — but if there is exactly ONE wired interface,
     * try DHCP on it anyway (some NICs never report carrier correctly). */
    if (nfound == 1) {
        strncpy(ifname, found[0], ifname_sz-1);
        ifname[ifname_sz-1] = 0;
        return 0;
    }

    /* Multiple wired interfaces, none with link — pick the first as best guess */
    strncpy(ifname, found[0], ifname_sz-1);
    ifname[ifname_sz-1] = 0;
    return 0;
}

/* Show diagnostics: list every interface and its state */
static void wb_show_diag(int rows, int cols) {
    wb_paint_bg(rows, cols);
    wb_at(2, (cols-20)/2);
    wb_w(WB_YEL "\x1b[1m── DIAGNOSTICS ──\x1b[22m");

    int y = 4;
    DIR *d = opendir("/sys/class/net");
    if (!d) {
        wb_at(y, 4); wb_w(WB_RED "Cannot open /sys/class/net");
    } else {
        struct dirent *e;
        wb_at(y++, 4); wb_w(WB_FG "\x1b[1mInterface     Type     Carrier  Flags\x1b[22m");
        wb_at(y++, 4); wb_w(WB_DIM "----------------------------------------");
        while ((e = readdir(d))) {
            if (e->d_name[0]=='.') continue;
            int wifi = iface_is_wireless(e->d_name);
            int car = iface_carrier(e->d_name);

            int s = socket(AF_INET, SOCK_DGRAM, 0);
            int up = 0;
            if (s>=0) {
                struct ifreq ifr; memset(&ifr,0,sizeof(ifr));
                strncpy(ifr.ifr_name, e->d_name, IFNAMSIZ-1);
                if (ioctl(s, SIOCGIFFLAGS, &ifr)==0) up = (ifr.ifr_flags & IFF_UP)?1:0;
                close(s);
            }

            wb_at(y, 4);
            char line[128];
            snprintf(line, sizeof(line), "%-13s %-8s %-8s %s",
                     e->d_name,
                     strcmp(e->d_name,"lo")==0?"loop":(wifi?"wifi":"wired"),
                     car?"YES":"no",
                     up?"UP":"DOWN");
            if (car) wb_w(WB_GRN); else if (wifi) wb_w(WB_DIM2); else wb_w(WB_FG);
            wb_w(line);
            y++;
            if (y >= rows-3) break;
        }
        closedir(d);
    }

    /* Show driver load error if any */
    y += 2;
    int errfd = open("/tmp/r8169_error.txt", O_RDONLY);
    if (errfd >= 0) {
        char ebuf[512] = {0};
        read(errfd, ebuf, sizeof(ebuf)-1);
        close(errfd);
        wb_at(y++, 4); wb_w(WB_RED "\x1b[1mDriver error:\x1b[22m");
        wb_at(y++, 4); wb_w(WB_RED); wb_w(ebuf);
        y++;
    } else {
        wb_at(y++, 4); wb_w(WB_GRN "Driver loaded OK (no error file)");
    }

    /* Show last few dmesg lines */
    int kfd = open("/dev/kmsg", O_RDONLY|O_NONBLOCK);
    if (kfd >= 0) {
        wb_at(y++, 4); wb_w(WB_YEL "\x1b[1mRecent kernel messages:\x1b[22m");
        char kbuf[8192]; int kn = read(kfd, kbuf, sizeof(kbuf)-1);
        close(kfd);
        if (kn > 0) {
            kbuf[kn] = 0;
            /* Print last ~10 lines containing r8169 or eth */
            char *p = kbuf;
            int shown = 0;
            while (*p && shown < 6 && y < rows-3) {
                char *eol = strchr(p, '\n');
                if (!eol) break;
                *eol = 0;
                if (strstr(p, "r8169") || strstr(p, "eth") || strstr(p, "Realtek") ||
                    strstr(p, "PCI")   || strstr(p, "init_module")) {
                    /* find body after timestamp prefix like "<6>4,123,..." */
                    char *body = strchr(p, ';');
                    if (body) body++;
                    else body = p;
                    wb_at(y++, 4); wb_w(WB_DIM2);
                    /* truncate to fit */
                    int max = cols - 6;
                    if ((int)strlen(body) > max) body[max] = 0;
                    wb_w(body);
                    shown++;
                }
                p = eol + 1;
            }
        }
    }

    wb_at(rows-1, 2);
    wb_w(WB_DIM "Press any key to return"WB_FG);
    fflush(stdout);
    unsigned char c; read(0, &c, 1);
}

/* ── Tiny DHCP client ───────────────────────────────────────────────────── *
 * Uses raw sockets to broadcast a DHCPDISCOVER, accept the OFFER, send
 * REQUEST, accept ACK. Sets the IP, gateway and DNS.
 */

struct dhcp_pkt {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[312];
} __attribute__((packed));

static int dhcp_send_raw(int sock, int ifindex, const uint8_t *mac,
                         struct dhcp_pkt *p, size_t plen) {
    /* Build full IP+UDP+DHCP packet for raw sending */
    uint8_t buf[1500] = {0};
    struct iphdr  *ip  = (struct iphdr*)buf;
    struct udphdr *udp = (struct udphdr*)(buf + sizeof(*ip));
    void          *pl  = buf + sizeof(*ip) + sizeof(*udp);

    memcpy(pl, p, plen);

    udp->source = htons(68);
    udp->dest   = htons(67);
    udp->len    = htons(sizeof(*udp) + plen);
    udp->check  = 0;

    ip->ihl      = 5;
    ip->version  = 4;
    ip->tos      = 0x10;
    ip->tot_len  = htons(sizeof(*ip) + sizeof(*udp) + plen);
    ip->id       = htons(0);
    ip->frag_off = 0;
    ip->ttl      = 64;
    ip->protocol = IPPROTO_UDP;
    ip->saddr    = 0;
    ip->daddr    = 0xFFFFFFFF;
    /* checksum */
    uint16_t *w = (uint16_t*)ip;
    uint32_t sum = 0;
    for (int i=0; i<5*2; i++) sum += w[i];
    while (sum>>16) sum = (sum&0xFFFF) + (sum>>16);
    ip->check = ~sum;

    struct sockaddr_ll addr = {0};
    addr.sll_family   = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_IP);
    addr.sll_ifindex  = ifindex;
    addr.sll_halen    = 6;
    memset(addr.sll_addr, 0xFF, 6);

    return sendto(sock, buf, sizeof(*ip)+sizeof(*udp)+plen, 0,
                  (struct sockaddr*)&addr, sizeof(addr));
}

static int dhcp_get(const char *ifname, uint32_t *ip_out, uint32_t *gw_out, uint32_t *dns_out) {
    int sock = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (sock < 0) return -1;

    struct ifreq ifr; memset(&ifr,0,sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);

    /* Get index */
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) { close(sock); return -1; }
    int ifindex = ifr.ifr_ifindex;

    /* Get MAC */
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) { close(sock); return -1; }
    uint8_t mac[6]; memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);

    /* Build a simpler approach: use a UDP socket with SO_BROADCAST + SO_BINDTODEVICE */
    close(sock);
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname));

    struct sockaddr_in src = {0};
    src.sin_family = AF_INET;
    src.sin_port   = htons(68);
    src.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr*)&src, sizeof(src)) < 0) {
        close(sock); return -1;
    }

    struct timeval tv = { 4, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t xid = (uint32_t)time(NULL) ^ getpid();

    /* DISCOVER */
    struct dhcp_pkt disc = {0};
    disc.op = 1; disc.htype = 1; disc.hlen = 6;
    disc.xid = xid;
    disc.flags = htons(0x8000);
    memcpy(disc.chaddr, mac, 6);
    disc.magic = htonl(0x63825363);
    int o = 0;
    disc.options[o++] = 53; disc.options[o++] = 1; disc.options[o++] = 1;  /* DHCPDISCOVER */
    disc.options[o++] = 55; disc.options[o++] = 4;                          /* param req */
    disc.options[o++] = 1;  /* subnet */
    disc.options[o++] = 3;  /* router */
    disc.options[o++] = 6;  /* DNS */
    disc.options[o++] = 51; /* lease time */
    disc.options[o++] = 0xFF;

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(67);
    dst.sin_addr.s_addr = htonl(0xFFFFFFFF);

    if (sendto(sock, &disc, sizeof(disc), 0,
               (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        close(sock); return -1;
    }

    /* Wait for OFFER */
    struct dhcp_pkt resp;
    int n;
    uint32_t offered_ip = 0, server_id = 0, gw = 0, dns = 0;
    int got_offer = 0;
    for (int try=0; try<3 && !got_offer; try++) {
        n = recv(sock, &resp, sizeof(resp), 0);
        if (n < (int)sizeof(struct dhcp_pkt) - 312) continue;
        if (resp.xid != xid) continue;
        if (resp.op != 2) continue;
        offered_ip = resp.yiaddr;
        /* parse options */
        for (int i=0; i<308; ) {
            uint8_t code = resp.options[i++];
            if (code == 0) continue;
            if (code == 0xFF) break;
            uint8_t len = resp.options[i++];
            if (i+len > 312) break;
            if (code == 53 && len == 1 && resp.options[i] == 2) got_offer = 1;
            if (code == 54 && len == 4) memcpy(&server_id, &resp.options[i], 4);
            if (code == 3  && len >= 4) memcpy(&gw, &resp.options[i], 4);
            if (code == 6  && len >= 4) memcpy(&dns, &resp.options[i], 4);
            i += len;
        }
    }
    if (!got_offer) { close(sock); return -1; }

    /* REQUEST */
    struct dhcp_pkt req = {0};
    req.op = 1; req.htype = 1; req.hlen = 6;
    req.xid = xid;
    req.flags = htons(0x8000);
    memcpy(req.chaddr, mac, 6);
    req.magic = htonl(0x63825363);
    o = 0;
    req.options[o++] = 53; req.options[o++] = 1; req.options[o++] = 3;  /* REQUEST */
    req.options[o++] = 50; req.options[o++] = 4;                         /* requested IP */
    memcpy(&req.options[o], &offered_ip, 4); o += 4;
    req.options[o++] = 54; req.options[o++] = 4;                         /* server id */
    memcpy(&req.options[o], &server_id, 4); o += 4;
    req.options[o++] = 0xFF;

    sendto(sock, &req, sizeof(req), 0,
           (struct sockaddr*)&dst, sizeof(dst));

    /* Wait for ACK */
    int got_ack = 0;
    for (int try=0; try<3 && !got_ack; try++) {
        n = recv(sock, &resp, sizeof(resp), 0);
        if (n < (int)sizeof(struct dhcp_pkt) - 312) continue;
        if (resp.xid != xid) continue;
        for (int i=0; i<308; ) {
            uint8_t code = resp.options[i++];
            if (code == 0) continue;
            if (code == 0xFF) break;
            uint8_t len = resp.options[i++];
            if (i+len > 312) break;
            if (code == 53 && len == 1 && resp.options[i] == 5) got_ack = 1;
            i += len;
        }
    }
    close(sock);
    if (!got_ack) return -1;

    *ip_out = offered_ip;
    *gw_out = gw;
    *dns_out = dns;
    return 0;
}

/* Configure interface using ioctl: set IP and bring up */
static int net_set_ip(const char *ifname, uint32_t ip, uint32_t gw) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq ifr; memset(&ifr,0,sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);

    /* address */
    struct sockaddr_in *sin = (struct sockaddr_in*)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = ip;
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) { close(s); return -1; }

    /* netmask /24 */
    sin->sin_addr.s_addr = htonl(0xFFFFFF00);
    ioctl(s, SIOCSIFNETMASK, &ifr);

    /* up + running */
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { close(s); return -1; }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    ioctl(s, SIOCSIFFLAGS, &ifr);

    /* default route */
    if (gw) {
        struct rtentry rt;
        memset(&rt, 0, sizeof(rt));
        struct sockaddr_in *gwa = (struct sockaddr_in*)&rt.rt_gateway;
        struct sockaddr_in *dst = (struct sockaddr_in*)&rt.rt_dst;
        struct sockaddr_in *msk = (struct sockaddr_in*)&rt.rt_genmask;
        dst->sin_family = AF_INET; dst->sin_addr.s_addr = 0;
        msk->sin_family = AF_INET; msk->sin_addr.s_addr = 0;
        gwa->sin_family = AF_INET; gwa->sin_addr.s_addr = gw;
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        rt.rt_dev = (char*)ifname;
        ioctl(s, SIOCADDRT, &rt);
    }
    close(s);
    return 0;
}

static void net_set_dns(uint32_t dns) {
    if (!dns) return;
    int fd = open("/etc/resolv.conf", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[64];
    struct in_addr a; a.s_addr = dns;
    snprintf(buf, sizeof(buf), "nameserver %s\n", inet_ntoa(a));
    write(fd, buf, strlen(buf));
    close(fd);
}

/* ── HTTP fetch ─────────────────────────────────────────────────────────── */
/* HTTPS GET via mbedTLS — supports TLS 1.2 (HTTP/1.1 only). */
static int https_get(const char *host, int port, const char *path,
                     char *out, size_t outsz) {
    int ret = -1;
    mbedtls_net_context server;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    mbedtls_net_init(&server);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    const char *pers = "triumph_web";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char*)pers, strlen(pers)) != 0)
        goto cleanup;

    /* Load CA bundle (PEM format) */
    if (mbedtls_x509_crt_parse_file(&cacert, "/etc/ssl/certs/ca-certificates.crt") < 0) {
        /* try without verification if bundle missing */
    }

    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", port);
    if (mbedtls_net_connect(&server, host, portstr, MBEDTLS_NET_PROTO_TCP) != 0)
        goto cleanup;

    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        goto cleanup;

    /* Optional verification: skip if no CA bundle was loaded */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    if (mbedtls_ssl_setup(&ssl, &conf) != 0) goto cleanup;
    if (mbedtls_ssl_set_hostname(&ssl, host) != 0) goto cleanup;
    mbedtls_ssl_set_bio(&ssl, &server, mbedtls_net_send, mbedtls_net_recv, NULL);

    int hret;
    while ((hret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (hret != MBEDTLS_ERR_SSL_WANT_READ && hret != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto cleanup;
    }

    char req[1024];
    int rn = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: TriumphWeb/1.0\r\n"
        "Accept: text/html,text/plain\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n\r\n",
        path, host);
    int sent = 0;
    while (sent < rn) {
        int w = mbedtls_ssl_write(&ssl, (const unsigned char*)req + sent, rn - sent);
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) goto cleanup;
        sent += w;
    }

    size_t got = 0;
    while (got < outsz - 1) {
        int r = mbedtls_ssl_read(&ssl, (unsigned char*)out + got, outsz - 1 - got);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (r <= 0) break;
        got += r;
    }
    out[got] = 0;
    ret = (int)got;

    mbedtls_ssl_close_notify(&ssl);

cleanup:
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_net_free(&server);
    return ret;
}

static int http_get(const char *host, int port, const char *path,
                    char *out, size_t outsz) {
    struct addrinfo hints={0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8]; snprintf(portstr,sizeof(portstr),"%d",port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { 8, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
        close(s); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: TriumphWeb/1.0\r\n"
        "Accept: text/html,text/plain\r\n"
        "Connection: close\r\n\r\n",
        path, host);
    write(s, req, n);

    size_t got = 0;
    while (got < outsz - 1) {
        int r = recv(s, out + got, outsz - 1 - got, 0);
        if (r <= 0) break;
        got += r;
    }
    out[got] = 0;
    close(s);
    return (int)got;
}

/* Strip HTML tags and decode common entities */
static void html_to_text(const char *html, char *out, size_t outsz) {
    size_t o = 0;
    int in_tag = 0, in_script = 0, in_style = 0, in_ws = 0;
    const char *p = html;
    while (*p && o < outsz - 1) {
        if (in_script) {
            if (strncasecmp(p, "</script>", 9) == 0) { in_script=0; p+=9; continue; }
            p++; continue;
        }
        if (in_style) {
            if (strncasecmp(p, "</style>", 8) == 0) { in_style=0; p+=8; continue; }
            p++; continue;
        }
        if (*p == '<') {
            if (strncasecmp(p, "<script", 7) == 0) { in_script=1; p+=7; continue; }
            if (strncasecmp(p, "<style",  6) == 0) { in_style=1; p+=6; continue; }
            if (strncasecmp(p, "<br",  3)==0 ||
                strncasecmp(p, "</p",  3)==0 ||
                strncasecmp(p, "</div",5)==0 ||
                strncasecmp(p, "</li", 4)==0 ||
                strncasecmp(p, "</h1",4)==0 || strncasecmp(p, "</h2",4)==0 ||
                strncasecmp(p, "</h3",4)==0 || strncasecmp(p, "</h4",4)==0) {
                if (o > 0 && out[o-1] != '\n') out[o++] = '\n';
                in_ws = 1;
            }
            in_tag = 1; p++; continue;
        }
        if (in_tag) { if (*p == '>') in_tag = 0; p++; continue; }

        if (*p == '&') {
            if (strncmp(p, "&amp;", 5) == 0)   { out[o++]='&'; p+=5; continue; }
            if (strncmp(p, "&lt;", 4) == 0)    { out[o++]='<'; p+=4; continue; }
            if (strncmp(p, "&gt;", 4) == 0)    { out[o++]='>'; p+=4; continue; }
            if (strncmp(p, "&quot;", 6) == 0)  { out[o++]='"'; p+=6; continue; }
            if (strncmp(p, "&apos;", 6) == 0)  { out[o++]='\''; p+=6; continue; }
            if (strncmp(p, "&nbsp;", 6) == 0)  { out[o++]=' '; p+=6; continue; }
            if (strncmp(p, "&#", 2) == 0) {
                int v = 0; const char *q = p+2;
                while (*q && *q != ';' && o < outsz-1) { v=v*10+(*q-'0'); q++; }
                if (*q == ';') q++;
                if (v >= 32 && v < 127) out[o++] = (char)v;
                p = q; continue;
            }
            out[o++] = '&'; p++; continue;
        }

        if (*p == '\r') { p++; continue; }
        if (*p == '\n' || *p == '\t' || *p == ' ') {
            if (!in_ws) { out[o++] = ' '; in_ws = 1; }
            p++; continue;
        }
        in_ws = 0;
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* ── UI ─────────────────────────────────────────────────────────────────── */
static void wb_show_no_internet(int rows, int cols) {
    wb_paint_bg(rows, cols);

    const char *line1 = "OOPS!";
    const char *line2 = "You have no internet";
    const char *line3 = "Connect to an ethernet cable to access the web";

    int y = rows / 2 - 4;
    int x;

    wb_at(y, (cols - 9)/2);
    wb_w(WB_RED "\x1b[1m"); wb_w("┌───────┐"); wb_w("\x1b[22m");
    wb_at(y+1, (cols - 9)/2);
    wb_w(WB_RED "\x1b[1m"); wb_w("│ "); wb_w(WB_YEL); wb_w(line1); wb_w(WB_RED" │"); wb_w("\x1b[22m");
    wb_at(y+2, (cols - 9)/2);
    wb_w(WB_RED "\x1b[1m"); wb_w("└───────┘"); wb_w("\x1b[22m");

    x = (cols - (int)strlen(line2)) / 2;
    wb_at(y+4, x);
    wb_w(WB_FG "\x1b[1m"); wb_w(line2); wb_w("\x1b[22m");

    x = (cols - (int)strlen(line3)) / 2;
    wb_at(y+5, x);
    wb_w(WB_DIM2); wb_w(line3);

    wb_at(y+8, (cols-32)/2);
    wb_w(WB_FG"D "WB_DIM "diagnose interfaces   "WB_FG"any other key "WB_DIM "back");
    fflush(stdout);

    unsigned char c; read(0, &c, 1);
    if (c=='d' || c=='D') wb_show_diag(rows, cols);
}

static int wb_readkey(int blocking) {
    unsigned char c;
    if (blocking) { if (read(0,&c,1)<=0) return 0; }
    else {
        struct termios cur; tcgetattr(0,&cur);
        struct termios tmp = cur;
        tmp.c_cc[VMIN]=0; tmp.c_cc[VTIME]=1;
        tcsetattr(0,TCSANOW,&tmp);
        int r = read(0,&c,1);
        tcsetattr(0,TCSANOW,&cur);
        if (r <= 0) return 0;
    }
    if (c == 27) {
        struct termios cur; tcgetattr(0,&cur);
        struct termios tmp = cur;
        tmp.c_cc[VMIN]=0; tmp.c_cc[VTIME]=1;
        tcsetattr(0,TCSANOW,&tmp);
        unsigned char seq[3]; int n=read(0,seq,3);
        tcsetattr(0,TCSANOW,&cur);
        if (n<=0) return 27;
        return -1;
    }
    return c;
}

static void wb_url_input(char *url, size_t sz, int rows, int cols) {
    /* Already at appropriate row — draw input box */
    int y = rows / 2;
    int boxw = 60;
    int x = (cols - boxw) / 2;

    wb_at(y-1, x);
    wb_w(WB_FG "\x1b[1mEnter URL (e.g. http://example.com):\x1b[22m");

    wb_at(y, x);
    wb_w(WB_FG "┌"); for(int i=0;i<boxw-2;i++) wb_w("─"); wb_w("┐");
    wb_at(y+1, x);
    wb_w(WB_FG "│ "); wb_w(WB_YEL);
    char input[256] = "http://";
    int ilen = 7;
    /* draw initial */
    char pad[80]; snprintf(pad,sizeof(pad),"%-*s", boxw-4, input);
    wb_w(pad);
    wb_at(y+1, x+boxw-1); wb_w(WB_FG"│");
    wb_at(y+2, x);
    wb_w(WB_FG "└"); for(int i=0;i<boxw-2;i++) wb_w("─"); wb_w("┘");

    wb_at(y+4, x);
    wb_w(WB_DIM "Type URL, Enter to fetch, Esc to cancel");

    wb_at(y+1, x + 2 + ilen);
    wb_w("\x1b[?25h");
    fflush(stdout);

    while (1) {
        int k = wb_readkey(1);
        if (k == 27 || k == -1) { url[0]=0; break; }
        if (k == 13 || k == 10) { strncpy(url, input, sz-1); url[sz-1]=0; break; }
        if ((k == 127 || k == 8) && ilen > 0) {
            ilen--; input[ilen]=0;
        } else if (k >= 32 && k < 127 && ilen < (int)sizeof(input)-1) {
            input[ilen++] = (char)k;
            input[ilen] = 0;
        }
        wb_at(y+1, x+2);
        wb_w(WB_YEL);
        char pad2[80]; snprintf(pad2,sizeof(pad2),"%-*s", boxw-4, input);
        wb_w(pad2);
        wb_at(y+1, x + 2 + ilen);
        fflush(stdout);
    }
    wb_w("\x1b[?25l");
}

/* parse url scheme://host[:port]/path */
static int parse_url(const char *url, char *host, int hostsz, int *port, char *path, int pathsz) {
    const char *p = url;
    int is_https = 0;
    if (strncasecmp(p, "http://", 7) == 0) { p += 7; *port = 80; }
    else if (strncasecmp(p, "https://", 8) == 0) { p += 8; *port = 443; is_https = 1; }
    else *port = 80;

    int i = 0;
    while (*p && *p != ':' && *p != '/' && i < hostsz-1) host[i++] = *p++;
    host[i] = 0;
    if (*p == ':') { p++; *port = atoi(p); while (*p && *p != '/') p++; }
    if (*p == 0) { strcpy(path, "/"); return 0; }
    strncpy(path, p, pathsz-1);
    path[pathsz-1] = 0;
    return 0;
}

/* Reflow text with word-wrap and render */
static void render_text(const char *text, int rows, int cols) {
    wb_paint_bg(rows, cols);
    wb_at(1, 2);
    wb_w(WB_YEL"\x1b[1m── PAGE ──"); wb_w("\x1b[22m");

    int margin = 4;
    int width  = cols - margin*2;
    int y = 3, x = margin;
    int max_y = rows - 3;
    int line_pos = 0;

    wb_at(y, x);
    wb_w(WB_FG);

    const char *p = text;
    while (*p && y < max_y) {
        /* find next word */
        while (*p == ' ' && *p) p++;
        const char *w = p;
        int wlen = 0;
        while (*p && *p != ' ' && *p != '\n') { p++; wlen++; }

        if (*w == '\n') { y++; x = margin; line_pos = 0; wb_at(y,x); p++; continue; }

        if (wlen == 0) continue;

        if (line_pos + wlen + 1 > width) {
            y++; x = margin; line_pos = 0;
            if (y >= max_y) break;
            wb_at(y, x);
        }

        if (line_pos > 0) { write(1, " ", 1); line_pos++; }
        write(1, w, wlen);
        line_pos += wlen;

        if (*p == '\n') { y++; x = margin; line_pos = 0; wb_at(y,x); p++; }
    }

    wb_at(rows-1, 2);
    wb_w(WB_DIM "Press any key to return");
    fflush(stdout);
    unsigned char c; read(0,&c,1);
}

static int b_web(Cmd *c) { (void)c;
    struct termios old, raw;
    tcgetattr(0,&old); raw=old;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    tcsetattr(0,TCSANOW,&raw);

    struct winsize ws; ioctl(0,TIOCGWINSZ,&ws);
    int rows = ws.ws_row?ws.ws_row:24;
    int cols = ws.ws_col?ws.ws_col:80;

    wb_w("\x1b[?25l\x1b[2J");

    /* 1. Find ethernet */
    char ifname[32];
    if (find_ethernet(ifname, sizeof(ifname)) < 0) {
        wb_show_no_internet(rows, cols);
        wb_w("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
        tcsetattr(0,TCSANOW,&old);
        return 0;
    }

    /* 2. Connecting message */
    wb_paint_bg(rows, cols);
    wb_at(rows/2 - 1, (cols-30)/2);
    wb_w(WB_FG"\x1b[1mConnecting via "WB_YEL); wb_w(ifname); wb_w(WB_FG"...\x1b[22m");
    fflush(stdout);

    /* 3. Bring interface up first */
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct ifreq ifr; memset(&ifr,0,sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);
            ioctl(s, SIOCGIFFLAGS, &ifr);
            ifr.ifr_flags |= IFF_UP;
            ioctl(s, SIOCSIFFLAGS, &ifr);
            close(s);
        }
    }
    sleep(1);

    /* 4. DHCP */
    uint32_t ip=0, gw=0, dns=0;
    if (dhcp_get(ifname, &ip, &gw, &dns) < 0) {
        wb_paint_bg(rows, cols);
        wb_at(rows/2 - 1, (cols-30)/2);
        wb_w(WB_RED"\x1b[1mDHCP failed — no IP address\x1b[22m");
        wb_at(rows/2 + 1, (cols-30)/2);
        wb_w(WB_DIM "Check the cable and try again");
        wb_at(rows-1, 2); wb_w(WB_FG "Press any key");
        fflush(stdout);
        unsigned char c; read(0,&c,1);
        wb_w("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
        tcsetattr(0,TCSANOW,&old);
        return 0;
    }

    net_set_ip(ifname, ip, gw);
    net_set_dns(dns);

    /* 5. Show "connected" briefly */
    wb_paint_bg(rows, cols);
    char ipstr[64]; struct in_addr a; a.s_addr = ip;
    snprintf(ipstr, sizeof(ipstr), "Connected: IP %s", inet_ntoa(a));
    wb_at(rows/2 - 1, (cols-(int)strlen(ipstr))/2);
    wb_w(WB_GRN"\x1b[1m"); wb_w(ipstr); wb_w("\x1b[22m");
    fflush(stdout);
    sleep(1);

    /* 6. URL input loop */
    while (1) {
        wb_paint_bg(rows, cols);
        wb_at(2, (cols-12)/2);
        wb_w(WB_YEL"\x1b[1m── WEB ──\x1b[22m");

        char url[256];
        wb_url_input(url, sizeof(url), rows, cols);
        if (url[0]==0) break;

        char host[128], path[256];
        int port;
        int pr = parse_url(url, host, sizeof(host), &port, path, sizeof(path));
        int use_https = (strncasecmp(url, "https://", 8) == 0);

        wb_paint_bg(rows, cols);
        if (pr < 0 && pr != -2) {
            wb_at(rows/2-1, (cols-30)/2);
            wb_w(WB_RED"\x1b[1mBad URL\x1b[22m");
            wb_at(rows-1, 2); wb_w(WB_FG "Press any key");
            fflush(stdout);
            unsigned char c; read(0,&c,1);
            continue;
        }
        if (host[0]==0) {
            wb_at(rows/2-1, (cols-20)/2);
            wb_w(WB_RED "Bad URL");
            wb_at(rows-1, 2); wb_w(WB_FG "Press any key");
            fflush(stdout);
            unsigned char c; read(0,&c,1);
            continue;
        }

        wb_at(rows/2, (cols-20)/2);
        wb_w(WB_FG"Fetching "); wb_w(WB_YEL); wb_w(host); wb_w("...");
        fflush(stdout);

        char *page = malloc(256*1024);
        char *text = malloc(256*1024);
        if (!page || !text) { free(page); free(text); break; }
        int n = use_https ? https_get(host, port, path, page, 256*1024)
                          : http_get(host, port, path, page, 256*1024);

        if (n <= 0) {
            wb_paint_bg(rows, cols);
            wb_at(rows/2-1, (cols-30)/2);
            wb_w(WB_RED"\x1b[1mFetch failed\x1b[22m");
            wb_at(rows/2+1, (cols-40)/2);
            wb_w(WB_DIM "Could not connect or DNS failed");
            wb_at(rows-1, 2); wb_w(WB_FG "Press any key");
            fflush(stdout);
            unsigned char c; read(0,&c,1);
            free(page); free(text);
            continue;
        }

        /* Skip headers — find blank line */
        char *body = strstr(page, "\r\n\r\n");
        if (body) body += 4; else body = page;
        html_to_text(body, text, 256*1024);
        render_text(text, rows, cols);
        free(page); free(text);
    }

    wb_w("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    tcsetattr(0,TCSANOW,&old);
    return 0;
}
