#include <sys/statfs.h>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <glob.h>
#include <ctype.h>
#include <stdarg.h>

#define SH_VERSION  "1.0.0"

#define SH_MAX_INPUT 4096
#define MAX_TOKENS  512
#define MAX_ARGS    256
#define MAX_PIPES   16
#define MAX_HISTORY 500
#define MAX_ALIASES 64

#define RST "\x1b[0m"
#define BLD "\x1b[1m"
#define RED "\x1b[38;5;196m"
#define GRN "\x1b[38;5;82m"
#define YLW "\x1b[38;5;226m"
#define CYN "\x1b[38;5;51m"
#define BLU "\x1b[38;5;39m"
#define MAG "\x1b[38;5;207m"
#define GRY "\x1b[38;5;245m"
#define WHT "\x1b[38;5;255m"
#define ORG "\x1b[38;5;208m"
#define PNK "\x1b[38;5;213m"

/* theme colour - used for prompt accents, changed by the `clr` builtin */
static char g_theme[32] = "\x1b[38;5;51m";  /* default cyan */

typedef struct { char *name; char *value; } Alias;

static char  *history[MAX_HISTORY];
static int    hist_count = 0, hist_pos = 0;
static Alias  aliases[MAX_ALIASES];
static int    alias_count = 0;
static int    last_exit = 0;
static int    running = 1;
static struct termios orig_termios;
static int    raw_mode_on = 0;

static void term_raw(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO|ICANON|ISIG|IEXTEN);
    raw.c_iflag &= ~(IXON|ICRNL|BRKINT|INPCK|ISTRIP);
    raw.c_cflag |=  CS8;
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_on = 1;
}
static void term_restore(void) {
    if (raw_mode_on) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); raw_mode_on=0; }
}
static int term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0&&ws.ws_col) return ws.ws_col;
    return 80;
}

static void sig_child(int s){(void)s; while(waitpid(-1,NULL,WNOHANG)>0);}
static void sig_int(int s){(void)s; write(1,"\n",1);}
static void signals_init(void){
    signal(SIGCHLD,sig_child); signal(SIGINT,sig_int);
    signal(SIGTSTP,SIG_IGN);  signal(SIGTTOU,SIG_IGN); signal(SIGTTIN,SIG_IGN);
}

static void hist_add(const char *l){
    if(!l||!*l) return;
    if(hist_count>0&&strcmp(history[hist_count-1],l)==0) return;
    if(hist_count>=MAX_HISTORY){free(history[0]);memmove(history,history+1,(MAX_HISTORY-1)*sizeof(char*));hist_count--;}
    history[hist_count++]=strdup(l);
    hist_pos=hist_count;
}

static void print_prompt(void){
    char cwd[512]; const char *home=getenv("HOME")?:"/root";
    if(!getcwd(cwd,sizeof(cwd))) strcpy(cwd,"?");
    char disp[512];
    size_t hl=strlen(home);
    if(strncmp(cwd,home,hl)==0&&(cwd[hl]=='/'||cwd[hl]=='\0'))
        snprintf(disp,sizeof(disp),"~%s",cwd+hl);
    else strncpy(disp,cwd,sizeof(disp));
    const char *user=getenv("USER")?:(getuid()==0?"root":"user");
    char host[64]; gethostname(host,sizeof(host));
    const char *arrow=(last_exit==0)?GRN:RED;
    const char *sym=(getuid()==0)?"#":"$";
    fprintf(stdout,"\r\n"BLD"%s╭─"RST BLD"["YLW"%s"GRY"@"BLU"%s"BLD"]"GRY"─"BLD"["GRN"%s"BLD"]"RST"\r\n",g_theme,user,host,disp);
    fprintf(stdout,BLD"%s╰─"RST"%s"BLD"%s "RST,g_theme,arrow,sym);
    fflush(stdout);
}

static const char *bltin[]={
    "alias","cat","cd","chmod","chown","clear","cp","date","df","du",
    "echo","env","eval","exec","exit","export","false","fetch","fg","file",
    "find","free","grep","head","help","history","hostname","id","jobs",
    "kill","ln","ls","mkdir","mv","printf","ps","pwd","read","rm","rmdir",
    "sleep","source","stat","tail","test","touch","true","uname","unalias",
    "unset","uptime","wc","whoami",NULL
};
static void complete(char *buf,int *len,int pos){
    int ws=pos;
    while(ws>0&&buf[ws-1]!=' ') ws--;
    char pre[256]; int pl=pos-ws;
    if(pl>=(int)sizeof(pre)) return;
    memcpy(pre,buf+ws,pl); pre[pl]='\0';
    char *cands[512]; int nc=0;
    for(int i=0;bltin[i]&&nc<511;i++)
        if(strncmp(bltin[i],pre,pl)==0) cands[nc++]=(char*)bltin[i];

    const char *dp="."; const char *fp=pre;
    char db[512]; char *sl=strrchr(pre,'/');
    if(sl){int dl=sl-pre+1;memcpy(db,pre,dl);db[dl]='\0';dp=db;fp=sl+1;}
    int fpl=strlen(fp);
    DIR *d=opendir(dp); char fb[256][256]; int fi=0;
    if(d){struct dirent *e;
        while((e=readdir(d))&&nc<511){
            if(e->d_name[0]=='.'&&(!fpl||fp[0]!='.')) continue;
            if(strncmp(e->d_name,fp,fpl)==0){
                if(sl) snprintf(fb[fi],256,"%.*s%s",(int)(sl-pre+1),pre,e->d_name);
                else   strncpy(fb[fi],e->d_name,255);
                cands[nc++]=fb[fi++];
            }
        }
        closedir(d);
    }
    if(!nc) return;
    if(nc==1){
        int sl2=strlen(cands[0]); int tl=*len-pos;
        memmove(buf+ws+sl2,buf+pos,tl);
        memcpy(buf+ws,cands[0],sl2);
        *len=ws+sl2+tl; buf[*len]='\0';
    } else {
        write(1,"\r\n",2);
        for(int i=0;i<nc;i++){write(1,cands[i],strlen(cands[i]));write(1,"  ",2);}
        write(1,"\r\n",2);
        int cp=pl;
        while(1){char c=cands[0][cp];if(!c)break;int ok=1;
            for(int i=1;i<nc;i++) if(cands[i][cp]!=c){ok=0;break;}
            if(!ok)break;cp++;}
        if(cp>pl){int ex=cp-pl;int tl=*len-pos;
            memmove(buf+pos+ex,buf+pos,tl);
            memcpy(buf+pos,cands[0]+pl,ex);
            *len+=ex;buf[*len]='\0';}
    }
}

static int triumph_readline(char *buf,int maxlen){
    int len=0,pos=0; buf[0]='\0'; hist_pos=hist_count;
    term_raw();
    while(1){
        unsigned char c;
        if(read(STDIN_FILENO,&c,1)<=0){term_restore();return -1;}
        if(c=='\r'||c=='\n'){term_restore();buf[len]='\0';write(1,"\r\n",2);return len;}
        if(c==3){term_restore();write(1,"^C\r\n",4);buf[0]='\0';return 0;}
        if(c==4&&len==0){term_restore();return -1;}
        if(c==127||c==8){
            if(pos>0){
                memmove(buf+pos-1,buf+pos,len-pos);len--;pos--;buf[len]='\0';
                write(1,"\x1b[D\x1b[P",7);
                write(1,buf+pos,len-pos);
                if(len-pos){char mv[16];snprintf(mv,16,"\x1b[%dD",len-pos);write(1,mv,strlen(mv));}
            }
        } else if(c==9){
            complete(buf,&len,pos);buf[len]='\0';
            write(1,"\r",1);print_prompt();write(1,buf,len);pos=len;
        } else if(c==27){
            unsigned char sq[2];
            if(read(STDIN_FILENO,&sq[0],1)<=0) continue;
            if(sq[0]!='[') continue;
            if(read(STDIN_FILENO,&sq[1],1)<=0) continue;
            if(sq[1]=='A'){
                if(hist_pos>0){hist_pos--;
                    if(pos){char mv[16];snprintf(mv,16,"\x1b[%dD",pos);write(1,mv,strlen(mv));}
                    write(1,"\x1b[K",3);
                    strncpy(buf,history[hist_pos],maxlen-1);len=pos=strlen(buf);write(1,buf,len);}
            } else if(sq[1]=='B'){
                if(pos){char mv[16];snprintf(mv,16,"\x1b[%dD",pos);write(1,mv,strlen(mv));}
                write(1,"\x1b[K",3);
                if(hist_pos<hist_count-1){hist_pos++;strncpy(buf,history[hist_pos],maxlen-1);}
                else{hist_pos=hist_count;buf[0]='\0';}
                len=pos=strlen(buf);write(1,buf,len);
            } else if(sq[1]=='C'){if(pos<len){pos++;write(1,"\x1b[C",3);}}
              else if(sq[1]=='D'){if(pos>0){pos--;write(1,"\x1b[D",3);}}
              else if(sq[1]=='3'){unsigned char t;read(STDIN_FILENO,&t,1);
                if(pos<len){memmove(buf+pos,buf+pos+1,len-pos-1);len--;buf[len]='\0';
                    write(1,"\x1b[P",3);write(1,buf+pos,len-pos);
                    if(len-pos){char mv[16];snprintf(mv,16,"\x1b[%dD",len-pos);write(1,mv,strlen(mv));}}}
        } else if(c>=32&&c<127){
            if(len<maxlen-1){
                memmove(buf+pos+1,buf+pos,len-pos);buf[pos]=c;len++;buf[len]='\0';
                write(1,buf+pos,len-pos);pos++;
                if(len-pos){char mv[16];snprintf(mv,16,"\x1b[%dD",len-pos);write(1,mv,strlen(mv));}}
        }
    }
}

static char *expand_vars(const char *s){
    char *out=malloc(SH_MAX_INPUT); int op=0;
    for(int i=0;s[i];){
        if(s[i]=='$'){i++;
            if(s[i]=='?'){char t[16];snprintf(t,16,"%d",last_exit);for(int k=0;t[k]&&op<SH_MAX_INPUT-1;k++)out[op++]=t[k];i++;}
            else if(s[i]=='$'){char t[16];snprintf(t,16,"%d",(int)getpid());for(int k=0;t[k]&&op<SH_MAX_INPUT-1;k++)out[op++]=t[k];i++;}
            else if(s[i]=='{'){i++;char n[128];int ni=0;while(s[i]&&s[i]!='}')n[ni++]=s[i++];n[ni]='\0';if(s[i]=='}'&&s[i])i++;
                const char *v=getenv(n);if(v)for(int k=0;v[k]&&op<SH_MAX_INPUT-1;k++)out[op++]=v[k];}
            else if(isalpha(s[i])||s[i]=='_'){char n[128];int ni=0;while(isalnum(s[i])||s[i]=='_')n[ni++]=s[i++];n[ni]='\0';
                const char *v=getenv(n);if(v)for(int k=0;v[k]&&op<SH_MAX_INPUT-1;k++)out[op++]=v[k];}
            else{if(op<SH_MAX_INPUT-1)out[op++]='$';}
        } else{if(op<SH_MAX_INPUT-1)out[op++]=s[i++];else i++;}
    }
    out[op]='\0';return out;
}

typedef struct {
    char *argv[MAX_ARGS]; int argc;
    char *in_file,*out_file,*err_file;
    int out_append,err_to_out,background;
} Cmd;

static char *tok_buf[MAX_TOKENS]; static int tok_n;
static void tokenise(char *line){
    tok_n=0; char *p=line;
    while(*p){
        while(*p==' '||*p=='\t')p++;
        if(!*p) break;
        char tmp[SH_MAX_INPUT]; int ti=0; int q=0; char qc=0;
        while(*p&&(q||(*p!=' '&&*p!='\t'))){
            if(!q&&(*p=='\''||*p=='"'))  {q=1;qc=*p++;}
            else if(q&&*p==qc)           {q=0;p++;}
            else{if(ti<SH_MAX_INPUT-1)tmp[ti++]=*p;p++;}
        }
        tmp[ti]='\0';
        if(tok_n<MAX_TOKENS-1) tok_buf[tok_n++]=strdup(tmp);
    }
}

static int parse_cmds(char *line,Cmd cmds[],int max){
    char *segs[MAX_PIPES]; int ns=0;
    char copy[SH_MAX_INPUT]; strncpy(copy,line,SH_MAX_INPUT-1);
    char *p=copy; segs[ns++]=p;
    for(;*p;p++){
        if(*p=='\''||*p=='"'){char q=*p++;while(*p&&*p!=q)p++;continue;}
        if(*p=='|'&&*(p+1)!='|'){*p='\0';p++;if(ns<max)segs[ns++]=p;}
    }
    for(int si=0;si<ns&&si<max;si++){
        Cmd *cmd=&cmds[si]; memset(cmd,0,sizeof(Cmd));
        tokenise(segs[si]);
        int ai=0;
        for(int ti=0;ti<tok_n;ti++){
            char *t=tok_buf[ti];
            if(strcmp(t,"&")==0){cmd->background=1;free(t);continue;}
            if(strcmp(t,">")==0&&ti+1<tok_n){cmd->out_file=tok_buf[++ti];cmd->out_append=0;continue;}
            if(strcmp(t,">>")==0&&ti+1<tok_n){cmd->out_file=tok_buf[++ti];cmd->out_append=1;continue;}
            if(strcmp(t,"<")==0&&ti+1<tok_n){cmd->in_file=tok_buf[++ti];continue;}
            if(strcmp(t,"2>")==0&&ti+1<tok_n){cmd->err_file=tok_buf[++ti];continue;}
            if(strcmp(t,"2>&1")==0){cmd->err_to_out=1;free(t);continue;}
            if(strchr(t,'$')){char *e=expand_vars(t);free(t);t=e;}
            if(strpbrk(t,"*?[")){
                glob_t g;memset(&g,0,sizeof(g));
                if(glob(t,GLOB_NOSORT|GLOB_TILDE,NULL,&g)==0){
                    for(size_t gi=0;gi<g.gl_pathc&&ai<MAX_ARGS-1;gi++) cmd->argv[ai++]=strdup(g.gl_pathv[gi]);
                    globfree(&g);free(t);continue;}
                globfree(&g);}
            if(ai<MAX_ARGS-1)cmd->argv[ai++]=t;else free(t);
        }
        cmd->argv[ai]=NULL;cmd->argc=ai;
    }
    return ns;
}

static const char *alias_lookup(const char *n){for(int i=0;i<alias_count;i++)if(strcmp(aliases[i].name,n)==0)return aliases[i].value;return NULL;}
static void alias_expand(char *line,int ml){
    char first[128];int fi=0;const char *p=line;
    while(*p==' '||*p=='\t')p++;
    while(*p&&*p!=' '&&*p!='\t'&&fi<127)first[fi++]=*p++;first[fi]='\0';
    const char *v=alias_lookup(first);if(!v)return;
    char rest[SH_MAX_INPUT];strncpy(rest,p,SH_MAX_INPUT-1);
    snprintf(line,ml,"%s%s",v,rest);
}

static void human_size(off_t s){
    const char *u[]={"B","K","M","G","T"};int i=0;double f=s;
    while(f>=1024.0&&i<4){f/=1024.0;i++;}
    if(i==0)printf("%s%4ld%s%s%-1s%s",CYN,(long)s,RST,WHT,u[i],RST);
    else     printf("%s%4.1f%s%s%-1s%s",CYN,f,RST,WHT,u[i],RST);
}
static void color_perms(mode_t m){
    char b[11]={S_ISDIR(m)?'d':S_ISLNK(m)?'l':'-',
        (m&S_IRUSR)?'r':'-',(m&S_IWUSR)?'w':'-',(m&S_IXUSR)?'x':'-',
        (m&S_IRGRP)?'r':'-',(m&S_IWGRP)?'w':'-',(m&S_IXGRP)?'x':'-',
        (m&S_IROTH)?'r':'-',(m&S_IWOTH)?'w':'-',(m&S_IXOTH)?'x':'-','\0'};
    printf("%s%s%s",GRY,b,RST);
}

static int run_line(char *line);

static int b_cd(Cmd *c){
    const char *d=c->argc>1?c->argv[1]:(getenv("HOME")?:"/");
    if(strcmp(d,"-")==0)d=getenv("OLDPWD")?:"/";
    char old[512];getcwd(old,sizeof(old));
    if(chdir(d)){perror("cd");return 1;}
    setenv("OLDPWD",old,1);
    char nw[512];getcwd(nw,sizeof(nw));setenv("PWD",nw,1);return 0;}

static int b_pwd(Cmd *c){(void)c;char w[512];getcwd(w,sizeof(w));printf("%s%s%s\n",GRN,w,RST);return 0;}
static int b_clear(Cmd *c){(void)c;printf("\x1b[2J\x1b[H");fflush(stdout);return 0;}
static int b_true(Cmd *c){(void)c;return 0;}
static int b_false(Cmd *c){(void)c;return 1;}
static int b_echo(Cmd *c){
    int nl=1,s=1;
    if(c->argc>1&&strcmp(c->argv[1],"-n")==0){nl=0;s++;}
    for(int i=s;i<c->argc;i++){if(i>s)putchar(' ');printf("%s",c->argv[i]);}
    if(nl)putchar('\n');return 0;}

static int b_ls(Cmd *c){
    const char *path=".";int all=0,lng=0,hum=0;
    for(int i=1;i<c->argc;i++){
        if(c->argv[i][0]=='-'){for(char *f=c->argv[i]+1;*f;f++){if(*f=='a'||*f=='A')all=1;if(*f=='l')lng=1;if(*f=='h')hum=1;}}
        else path=c->argv[i];}
    DIR *d=opendir(path);if(!d){perror("ls");return 1;}
    struct dirent **ents=NULL;int ec=0;struct dirent *e;
    while((e=readdir(d))){if(!all&&e->d_name[0]=='.')continue;ents=realloc(ents,(ec+1)*sizeof(struct dirent*));ents[ec]=malloc(sizeof(struct dirent));*ents[ec++]=*e;}
    closedir(d);
    for(int a=0;a<ec-1;a++)for(int b=a+1;b<ec;b++)if(strcasecmp(ents[a]->d_name,ents[b]->d_name)>0){struct dirent *t=ents[a];ents[a]=ents[b];ents[b]=t;}
    int cols=term_width(),cu=0;
    for(int i=0;i<ec;i++){
        char full[1024];snprintf(full,1024,"%s/%s",path,ents[i]->d_name);
        struct stat st;lstat(full,&st);
        if(lng){color_perms(st.st_mode);printf(" ");if(hum)human_size(st.st_size);else printf("%s%8ld%s",CYN,(long)st.st_size,RST);printf(" ");}
        if(S_ISDIR(st.st_mode))       printf(BLD BLU"%s"RST,ents[i]->d_name);
        else if(S_ISLNK(st.st_mode))  printf(CYN"%s"RST,ents[i]->d_name);
        else if(st.st_mode&0111)       printf(GRN"%s"RST,ents[i]->d_name);
        else                            printf(WHT"%s"RST,ents[i]->d_name);
        if(lng)printf("\n");else{printf("  ");cu+=strlen(ents[i]->d_name)+2;if(cu+20>cols){printf("\n");cu=0;}}
        free(ents[i]);}
    if(!lng&&cu)printf("\n");free(ents);return 0;}

static int b_cat(Cmd *c){
    if(c->argc<2){char buf[4096];int n;while((n=read(0,buf,sizeof(buf)))>0)write(1,buf,n);return 0;}
    for(int i=1;i<c->argc;i++){FILE *f=fopen(c->argv[i],"r");if(!f){perror(c->argv[i]);continue;}
        char buf[4096];size_t n;while((n=fread(buf,1,sizeof(buf),f))>0)fwrite(buf,1,n,stdout);fclose(f);}return 0;}

static int b_grep(Cmd *c){
    int ni=1,inv=0,ic=0,ln=0;const char *pat=NULL;
    for(;ni<c->argc;ni++){
        if(strcmp(c->argv[ni],"-n")==0)ln=1;
        else if(strcmp(c->argv[ni],"-i")==0)ic=1;
        else if(strcmp(c->argv[ni],"-v")==0)inv=1;
        else if(c->argv[ni][0]!='-')break;}
    if(ni>=c->argc){fprintf(stderr,"grep: pattern required\n");return 1;}
    pat=c->argv[ni++];
    FILE *fs[64];int nf=0;
    for(;ni<c->argc;ni++){FILE *f=fopen(c->argv[ni],"r");if(f)fs[nf++]=f;else perror(c->argv[ni]);}
    if(!nf)fs[nf++]=stdin;
    for(int fi=0;fi<nf;fi++){char line[4096];int lnum=0;
        while(fgets(line,sizeof(line),fs[fi])){lnum++;
            char cl[4096],cp2[256];strncpy(cl,line,4095);strncpy(cp2,pat,255);
            if(ic){for(char *x=cl;*x;x++)*x=tolower(*x);for(char *x=cp2;*x;x++)*x=tolower(*x);}
            int m=(strstr(cl,cp2)!=NULL);if(inv)m=!m;
            if(m){if(ln)printf("%s%d%s:",YLW,lnum,RST);
                char *h=line,*fnd;
                while((fnd=strstr(h,pat))!=NULL){fwrite(h,1,fnd-h,stdout);printf(RED BLD"%s"RST,pat);h=fnd+strlen(pat);}
                printf("%s",h);}}
        if(fs[fi]!=stdin)fclose(fs[fi]);}return 0;}

static int b_head(Cmd *c){
    int n=10;const char *fn=NULL;
    for(int i=1;i<c->argc;i++){if(strcmp(c->argv[i],"-n")==0&&i+1<c->argc)n=atoi(c->argv[++i]);else if(c->argv[i][0]=='-'&&isdigit(c->argv[i][1]))n=atoi(c->argv[i]+1);else fn=c->argv[i];}
    FILE *f=fn?fopen(fn,"r"):stdin;if(!f){perror(fn);return 1;}
    char l[4096];int cnt=0;while(cnt<n&&fgets(l,sizeof(l),f)){{printf("%s",l);cnt++;}}
    if(fn)fclose(f);return 0;}

static int b_tail(Cmd *c){
    int n=10;const char *fn=NULL;
    for(int i=1;i<c->argc;i++){if(strcmp(c->argv[i],"-n")==0&&i+1<c->argc)n=atoi(c->argv[++i]);else if(c->argv[i][0]=='-'&&isdigit(c->argv[i][1]))n=atoi(c->argv[i]+1);else fn=c->argv[i];}
    FILE *f=fn?fopen(fn,"r"):stdin;if(!f){perror(fn);return 1;}
    char **ring=malloc(n*sizeof(char*));for(int i=0;i<n;i++)ring[i]=NULL;
    int ri=0;char l[4096];
    while(fgets(l,sizeof(l),f)){free(ring[ri]);ring[ri]=strdup(l);ri=(ri+1)%n;}
    if(fn)fclose(f);
    for(int i=0;i<n;i++){int idx=(ri+i)%n;if(ring[idx])printf("%s",ring[idx]);free(ring[idx]);}
    free(ring);return 0;}

static int b_wc(Cmd *c){
    int dl=0,dw=0,dc=0;const char *fn=NULL;
    for(int i=1;i<c->argc;i++){if(strcmp(c->argv[i],"-l")==0)dl=1;else if(strcmp(c->argv[i],"-w")==0)dw=1;else if(strcmp(c->argv[i],"-c")==0)dc=1;else fn=c->argv[i];}
    if(!dl&&!dw&&!dc)dl=dw=dc=1;
    FILE *f=fn?fopen(fn,"r"):stdin;if(!f){perror(fn);return 1;}
    long lines=0,words=0,chars=0;int inw=0,ch;
    while((ch=fgetc(f))!=EOF){chars++;if(ch=='\n')lines++;if(isspace(ch))inw=0;else if(!inw){inw=1;words++;}}
    if(fn)fclose(f);
    if(dl)printf("%s%ld%s ",CYN,lines,RST);if(dw)printf("%s%ld%s ",YLW,words,RST);if(dc)printf("%s%ld%s ",GRN,chars,RST);
    if(fn)printf("%s",fn);printf("\n");return 0;}

static void find_r(const char *base,const char *pat){
    DIR *d=opendir(base);if(!d)return;struct dirent *e;
    while((e=readdir(d))){if(strcmp(e->d_name,".")==0||strcmp(e->d_name,"..")==0)continue;
        char p[1024];snprintf(p,1024,"%s/%s",base,e->d_name);
        if(!pat||strstr(e->d_name,pat))printf("%s%s%s\n",GRY,p,RST);
        if(e->d_type==DT_DIR)find_r(p,pat);}
    closedir(d);}
static int b_find(Cmd *c){
    const char *r=".";const char *n=NULL;
    for(int i=1;i<c->argc;i++){if(strcmp(c->argv[i],"-name")==0&&i+1<c->argc)n=c->argv[++i];else if(c->argv[i][0]!='-')r=c->argv[i];}
    find_r(r,n);return 0;}

static int b_mkdir(Cmd *c){
    int p=0;for(int i=1;i<c->argc;i++){if(strcmp(c->argv[i],"-p")==0){p=1;continue;}
        if(p){char t[512];strncpy(t,c->argv[i],511);for(char *x=t+1;*x;x++)if(*x=='/'){*x='\0';mkdir(t,0755);*x='/';}}
        if(mkdir(c->argv[i],0755)&&errno!=EEXIST)perror(c->argv[i]);}return 0;}
static int b_rmdir(Cmd *c){for(int i=1;i<c->argc;i++)if(rmdir(c->argv[i]))perror(c->argv[i]);return 0;}
static int b_rm(Cmd *c){
    int rec=0;for(int i=1;i<c->argc;i++){
        if(c->argv[i][0]=='-'){if(strstr(c->argv[i],"r"))rec=1;continue;}
        if(unlink(c->argv[i])&&errno==EISDIR){if(rec){char t[512];snprintf(t,512,"rm -rf %s",c->argv[i]);system(t);}else fprintf(stderr,"rm: %s is a directory\n",c->argv[i]);}}return 0;}
static int b_mv(Cmd *c){if(c->argc<3){fprintf(stderr,"mv: need src dst\n");return 1;}if(rename(c->argv[1],c->argv[2])){perror("mv");return 1;}return 0;}
static int b_cp(Cmd *c){
    if(c->argc<3){fprintf(stderr,"cp: need src dst\n");return 1;}
    FILE *s=fopen(c->argv[1],"r");if(!s){perror(c->argv[1]);return 1;}
    FILE *d=fopen(c->argv[2],"w");if(!d){perror(c->argv[2]);fclose(s);return 1;}
    char buf[65536];size_t n;while((n=fread(buf,1,sizeof(buf),s))>0)fwrite(buf,1,n,d);
    fclose(s);fclose(d);return 0;}
static int b_touch(Cmd *c){for(int i=1;i<c->argc;i++){int fd=open(c->argv[i],O_WRONLY|O_CREAT,0644);if(fd<0){perror(c->argv[i]);continue;}close(fd);}return 0;}
static int b_chmod(Cmd *c){if(c->argc<3){fprintf(stderr,"chmod: need mode file\n");return 1;}mode_t m=(mode_t)strtol(c->argv[1],NULL,8);for(int i=2;i<c->argc;i++)if(chmod(c->argv[i],m))perror(c->argv[i]);return 0;}
static int b_ln(Cmd *c){int s=0,a=1;if(c->argc>1&&strcmp(c->argv[1],"-s")==0){s=1;a++;}if(c->argc-a<2){fprintf(stderr,"ln: need src dst\n");return 1;}return s?symlink(c->argv[a],c->argv[a+1]):link(c->argv[a],c->argv[a+1]);}
static int b_stat(Cmd *c){
    for(int i=1;i<c->argc;i++){struct stat st;if(lstat(c->argv[i],&st)){perror(c->argv[i]);continue;}
        printf(BLD"  File: "RST WHT"%s"RST"\n",c->argv[i]);printf(BLD"  Size: "RST CYN"%ld"RST"\n",(long)st.st_size);
        color_perms(st.st_mode);printf(" (%04o)\n",(unsigned)st.st_mode&07777);}return 0;}
static int b_file(Cmd *c){
    for(int i=1;i<c->argc;i++){struct stat st;if(lstat(c->argv[i],&st)){perror(c->argv[i]);continue;}
        printf("%s%s%s: ",YLW,c->argv[i],RST);
        if(S_ISDIR(st.st_mode))puts("directory");
        else if(S_ISLNK(st.st_mode)){char t[256];int n=readlink(c->argv[i],t,255);t[n<0?0:n]='\0';printf("symbolic link -> %s\n",t);}
        else{FILE *f=fopen(c->argv[i],"r");if(!f){puts("unknown");continue;}
            unsigned char h[4];fread(h,1,4,f);fclose(f);
            if(h[0]==0x7f&&h[1]=='E'&&h[2]=='L'&&h[3]=='F')puts("ELF binary");
            else if(h[0]=='#'&&h[1]=='!')puts("script");
            else puts("data");}}return 0;}

static int b_du(Cmd *c){
    const char *p=c->argc>1?c->argv[1]:".";

    struct stat st;if(stat(p,&st)){perror(p);return 1;}
    human_size(st.st_size);printf("\t%s\n",p);return 0;}

static int b_df(Cmd *c){(void)c;
    FILE *f=fopen("/proc/mounts","r");if(!f){perror("df");return 1;}
    printf(BLD"%-20s %8s %8s %8s %5s\n"RST,"Filesystem","Size","Used","Avail","Use%");
    char dev[128],mp[256],fs[64],opts[256];int fr,ps2;
    while(fscanf(f,"%127s %255s %63s %255s %d %d",dev,mp,fs,opts,&fr,&ps2)==6){
        struct statfs sfs;if(statfs(mp,&sfs))continue;
        long tot=(long)sfs.f_blocks*(sfs.f_bsize/1024);
        long av=(long)sfs.f_bavail*(sfs.f_bsize/1024);
        long us=tot-av;if(!tot)continue;
        int pct=(int)(100.0*us/tot);
        const char *pc=pct>90?RED:pct>70?YLW:GRN;
        printf("%-20s ",dev);human_size(tot*1024);printf(" ");human_size(us*1024);printf(" ");human_size(av*1024);printf(" %s%4d%%%s\n",pc,pct,RST);}
    fclose(f);return 0;}

static int b_ps(Cmd *c){(void)c;
    DIR *d=opendir("/proc");if(!d){perror("ps");return 1;}
    printf(BLD"%6s  %-10s  %s\n"RST,"PID","STAT","CMD");
    struct dirent *e;
    while((e=readdir(d))){if(!isdigit(e->d_name[0]))continue;
        char sp[512];snprintf(sp,512,"/proc/%s/status",e->d_name);
        FILE *sf=fopen(sp,"r");if(!sf)continue;
        char name[64]="?",state[8]="?",line[256];
        while(fgets(line,sizeof(line),sf)){if(strncmp(line,"Name:",5)==0)sscanf(line,"Name:\t%63s",name);if(strncmp(line,"State:",6)==0)sscanf(line,"State:\t%7s",state);}
        fclose(sf);printf("%s%6s%s  %-10s  %s%s%s\n",YLW,e->d_name,RST,state,WHT,name,RST);}
    closedir(d);return 0;}

static int b_kill(Cmd *c){
    int sig=SIGTERM,ai=1;if(c->argc>1&&c->argv[1][0]=='-'){sig=atoi(c->argv[1]+1);ai++;}
    for(int i=ai;i<c->argc;i++)if(kill(atoi(c->argv[i]),sig))perror("kill");return 0;}

static int b_sleep(Cmd *c){if(c->argc<2)return 1;usleep((useconds_t)(atof(c->argv[1])*1e6));return 0;}
static int b_date(Cmd *c){(void)c;time_t t=time(NULL);printf("%s%s%s\n",CYN,ctime(&t),RST);return 0;}
static int b_uname(Cmd *c){struct utsname u;uname(&u);int a=c->argc>1&&strcmp(c->argv[1],"-a")==0;
    if(a)printf("Triumph %s Triumph-1.0.0 Triumph-OS %s\n",u.nodename,u.machine);else printf("Triumph\n");return 0;}
static int b_hostname(Cmd *c){if(c->argc>1){sethostname(c->argv[1],strlen(c->argv[1]));return 0;}char h[256];gethostname(h,sizeof(h));printf("%s%s%s\n",CYN,h,RST);return 0;}
static int b_whoami(Cmd *c){(void)c;struct passwd *pw=getpwuid(getuid());printf("%s%s%s\n",GRN,pw?pw->pw_name:"?",RST);return 0;}
static int b_id(Cmd *c){(void)c;uid_t uid=getuid();gid_t gid=getgid();struct passwd *pw=getpwuid(uid);struct group *gr=getgrgid(gid);printf("%suid%s=%s%d%s(%s) %sgid%s=%s%d%s(%s)\n",YLW,RST,CYN,uid,RST,pw?pw->pw_name:"?",YLW,RST,CYN,gid,RST,gr?gr->gr_name:"?");return 0;}
static int b_uptime(Cmd *c){(void)c;struct sysinfo si;sysinfo(&si);long h=si.uptime/3600,m=(si.uptime%3600)/60,s=si.uptime%60;printf("%suptime:%s %s%ldh %ldm %lds%s  load: %s%.2f %.2f %.2f%s\n",YLW,RST,CYN,h,m,s,RST,GRN,si.loads[0]/65536.0,si.loads[1]/65536.0,si.loads[2]/65536.0,RST);return 0;}
static int b_free(Cmd *c){(void)c;struct sysinfo si;sysinfo(&si);unsigned long u=si.mem_unit;unsigned long tot=si.totalram*u/1024/1024,usd=(si.totalram-si.freeram)*u/1024/1024,fr=si.freeram*u/1024/1024;
    printf(BLD"%-8s %8s %8s %8s\n"RST,"","total","used","free");printf("%-8s %s%8lu%s %s%8lu%s %s%8lu%s MiB\n","Mem:",CYN,tot,RST,YLW,usd,RST,GRN,fr,RST);return 0;}

static int b_history(Cmd *c){int n=c->argc>1?atoi(c->argv[1]):hist_count;int s=hist_count-n;if(s<0)s=0;for(int i=s;i<hist_count;i++)printf("%s%4d%s  %s\n",YLW,i+1,RST,history[i]);return 0;}
static int b_alias(Cmd *c){
    if(c->argc==1){for(int i=0;i<alias_count;i++)printf("%salias%s %s='%s'\n",CYN,RST,aliases[i].name,aliases[i].value);return 0;}
    char *arg=c->argv[1];char *eq=strchr(arg,'=');
    if(!eq){const char *v=alias_lookup(arg);if(v)printf("alias %s='%s'\n",arg,v);return 0;}
    *eq='\0';char *val=eq+1;
    if((*val=='\''&&val[strlen(val)-1]=='\'')||(*val=='"'&&val[strlen(val)-1]=='"')){val++;val[strlen(val)-1]='\0';}
    for(int i=0;i<alias_count;i++)if(strcmp(aliases[i].name,arg)==0){free(aliases[i].value);aliases[i].value=strdup(val);return 0;}
    if(alias_count<MAX_ALIASES){aliases[alias_count].name=strdup(arg);aliases[alias_count].value=strdup(val);alias_count++;}return 0;}
static int b_unalias(Cmd *c){if(c->argc<2)return 1;for(int i=0;i<alias_count;i++)if(strcmp(aliases[i].name,c->argv[1])==0){free(aliases[i].name);free(aliases[i].value);memmove(&aliases[i],&aliases[i+1],(alias_count-i-1)*sizeof(Alias));alias_count--;return 0;}return 1;}
static int b_export(Cmd *c){if(c->argc==1){extern char **environ;for(char **e=environ;*e;e++)printf("%sexport%s %s\n",CYN,RST,*e);return 0;}for(int i=1;i<c->argc;i++){char *eq=strchr(c->argv[i],'=');if(eq)putenv(strdup(c->argv[i]));else{const char *v=getenv(c->argv[i]);if(v)setenv(c->argv[i],v,1);}}return 0;}
static int b_unset(Cmd *c){for(int i=1;i<c->argc;i++)unsetenv(c->argv[i]);return 0;}
static int b_env(Cmd *c){(void)c;extern char **environ;for(char **e=environ;*e;e++)printf("%s\n",*e);return 0;}
static int b_read(Cmd *c){char buf[1024];term_restore();if(!fgets(buf,sizeof(buf),stdin))return 1;buf[strcspn(buf,"\n")]='\0';if(c->argc>1)setenv(c->argv[1],buf,1);return 0;}
static int b_source(Cmd *c){if(c->argc<2)return 1;FILE *f=fopen(c->argv[1],"r");if(!f){perror(c->argv[1]);return 1;}char l[SH_MAX_INPUT];while(fgets(l,sizeof(l),f)){l[strcspn(l,"\n")]='\0';if(*l&&*l!='#')run_line(l);}fclose(f);return 0;}
static int b_test(Cmd *c){
    int ac=c->argc;if(ac>1&&strcmp(c->argv[ac-1],"]")==0)ac--;
    if(ac<2)return 1;if(ac==2)return strlen(c->argv[1])>0?0:1;
    if(ac==3){char *op=c->argv[1],*a=c->argv[2];struct stat st;
        if(strcmp(op,"-f")==0)return stat(a,&st)?1:S_ISREG(st.st_mode)?0:1;
        if(strcmp(op,"-d")==0)return stat(a,&st)?1:S_ISDIR(st.st_mode)?0:1;
        if(strcmp(op,"-e")==0)return stat(a,&st)?1:0;
        if(strcmp(op,"-z")==0)return strlen(a)==0?0:1;
        if(strcmp(op,"-n")==0)return strlen(a)>0?0:1;
        if(strcmp(op,"-r")==0)return access(a,R_OK)?1:0;
        if(strcmp(op,"-w")==0)return access(a,W_OK)?1:0;
        if(strcmp(op,"-x")==0)return access(a,X_OK)?1:0;}
    if(ac==4){char *a=c->argv[1],*op=c->argv[2],*b=c->argv[3];
        if(strcmp(op,"=")==0||strcmp(op,"==")==0)return strcmp(a,b)?1:0;
        if(strcmp(op,"!=")==0)return strcmp(a,b)?0:1;
        long ai=atol(a),bi=atol(b);
        if(strcmp(op,"-eq")==0)return ai==bi?0:1;if(strcmp(op,"-ne")==0)return ai!=bi?0:1;
        if(strcmp(op,"-lt")==0)return ai<bi?0:1; if(strcmp(op,"-le")==0)return ai<=bi?0:1;
        if(strcmp(op,"-gt")==0)return ai>bi?0:1; if(strcmp(op,"-ge")==0)return ai>=bi?0:1;}
    return 1;}

static int b_fetch(Cmd *c){(void)c;
    struct utsname uts;uname(&uts);struct sysinfo si;sysinfo(&si);
    char host[128];gethostname(host,sizeof(host));
    char cpu[128]="Unknown CPU";
    {FILE *f=fopen("/proc/cpuinfo","r");if(f){char l[256];while(fgets(l,256,f)){if(strncmp(l,"model name",10)==0){char *co=strchr(l,':');if(co){co++;while(*co==' ')co++;strncpy(cpu,co,127);cpu[strcspn(cpu,"\n")]='\0';break;}}fclose(f);}}
    }
    int cores=0;{FILE *f=fopen("/proc/cpuinfo","r");if(f){char l[256];while(fgets(l,256,f))if(strncmp(l,"processor",9)==0)cores++;fclose(f);}}
    unsigned long mt=si.totalram*si.mem_unit/1024/1024,mu=(si.totalram-si.freeram)*si.mem_unit/1024/1024;
    long uh=si.uptime/3600,um=(si.uptime%3600)/60;
    const char *user=getenv("USER")?:(getuid()==0?"root":"user");
    const char *logo[]={
        " "CYN BLD"TTTTTTTTTTTTTTTTTTTTTTT"RST,
        " "CYN BLD"T:::::::::::::::::::::T"RST,
        " "CYN BLD"T:::::::::::::::::::::T"RST,
        " "CYN BLD"T:::::TT:::::::TT:::::T"RST,
        " "CYN BLD"TTTTTT  T:::::T  TTTTTT"RST,
        " "CYN BLD"        T:::::T        "RST,
        " "CYN BLD"        T:::::T        "RST,
        " "CYN BLD"      TT:::::::TT      "RST,
        " "CYN BLD"      T:::::::::T      "RST,
        " "CYN BLD"      T:::::::::T      "RST,
        " "CYN BLD"      TTTTTTTTTTT      "RST,
        NULL};
    char info[12][256];int ni=0;
    snprintf(info[ni++],256,"  "BLD WHT"%s"RST BLD GRY"@"RST BLD WHT"%s"RST,user,host);
    snprintf(info[ni++],256,"  "GRY"─────────────────────────"RST);
    snprintf(info[ni++],256,"  "BLD CYN"OS"RST": "WHT"Triumph OS 1.0 (Live)"RST);
    snprintf(info[ni++],256,"  "BLD CYN"Kernel"RST": "GRN"Triumph v%s"RST,SH_VERSION);
    snprintf(info[ni++],256,"  "BLD CYN"Shell"RST": "GRN"triumph v%s"RST,SH_VERSION);
    snprintf(info[ni++],256,"  "BLD CYN"CPU"RST": "GRN"%s (%d)"RST,cpu,cores);
    snprintf(info[ni++],256,"  "BLD CYN"Memory"RST": %s%lu%s/%s%lu%s MiB",(mt&&mu*100/mt>80)?RED:GRN,mu,RST,CYN,mt,RST);
    snprintf(info[ni++],256,"  "BLD CYN"Uptime"RST": "GRN"%ldh %ldm"RST,uh,um);

    char bar[256]="  ";
    const char *cs[]={"\x1b[40m","\x1b[41m","\x1b[42m","\x1b[43m","\x1b[44m","\x1b[45m","\x1b[46m","\x1b[47m"};
    for(int i=0;i<8;i++){strcat(bar,cs[i]);strcat(bar,"   ");}strcat(bar,RST);
    strncpy(info[ni++],bar,255);
    printf("\n");
    int lc=0;while(logo[lc])lc++;int rows=lc>ni?lc:ni;
    for(int r=0;r<rows;r++){if(r<lc)printf("%s",logo[r]);if(r<ni)printf("  %s",info[r]);printf("\n");}
    printf("\n");return 0;}

static int b_help(Cmd *c){(void)c;
    printf("\n%s%s Triumph OS Shell v%s %s\n\n",BLD,CYN,SH_VERSION,RST);
    printf(BLD"Built-in commands:\n"RST);
    const char *h[][2]={
        {"cd","change directory"},{"pwd","print working dir"},{"ls [-alh]","list files"},
        {"cat","print file"},{"echo","print text"},{"grep","search text"},
        {"head/tail","first/last lines"},{"wc","count lines/words/chars"},
        {"find","find files"},{"du","disk usage"},{"df","disk free"},
        {"stat","file metadata"},{"file","identify file type"},
        {"mkdir/rmdir","create/remove dirs"},{"rm","remove files"},
        {"mv/cp","move/copy files"},{"touch","create file"},{"chmod","change perms"},
        {"ln","create links"},{"ps","list processes"},{"kill","send signal"},
        {"sleep","wait N seconds"},{"date","print date"},{"uname","system info"},
        {"hostname","get/set hostname"},{"whoami/id","user info"},
        {"uptime","system uptime"},{"free","memory usage"},
        {"history","command history"},{"export/unset","env variables"},
        {"alias/unalias","manage aliases"},{"read","read input"},
        {"source/.","run script"},{"test/[","evaluate conditions"},
        {"clear","clear screen"},{"nano/edit","text editor"},{"snake","snake game"},{"tetris","tetris game"},{"pongy","2-player pong vs AI"},{"chicken","endless runner (SPACE to jump)"},{"menu","fullscreen launcher menu"},{"files","file explorer"},{"web","HTTP browser (ethernet)"},{"clr -[r/o/y/g/b/m/c/p/w/k]","change prompt colour"},{"calc","calculator (calc 2+2)"},{"figlet","big ASCII text"},{"poweroff","power off the machine"},{"fetch","system info panel"},
        {"help","this help"},{"exit","quit shell"},{NULL,NULL}};
    for(int i=0;h[i][0];i++) printf("  %s%-20s%s %s%s%s\n",GRN,h[i][0],RST,GRY,h[i][1],RST);
    printf("\n"GRY"Pipes: cmd1 | cmd2    Redirect: > >> < 2>    Background: cmd &\n\n"RST);
    return 0;}

#include <sys/reboot.h>
#include "beep.c"
static int b_poweroff(Cmd *c){(void)c;
    pc_play(SND_SHUTDOWN);
    sync();
    printf("\n\x1b[1m\x1b[38;5;196mPowering off...\x1b[0m\n");
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 0;}
static int b_reboot(Cmd *c){(void)c;
    pc_play(SND_SHUTDOWN);
    sync();
    printf("\n\x1b[1m\x1b[38;5;226mRebooting...\x1b[0m\n");
    fflush(stdout);
    reboot(RB_AUTOBOOT);
    return 0;}

#include "editor.c"
#include "snake.c"
#include "tetris.c"
#include "pongy.c"
#include "chicken.c"
#include "calc_ui.c"
#include "files.c"
#include "web.c"
#include "menu.c"
#include "tools.c"

static int b_clr(Cmd *c) {
    if (c->argc < 2) {
        printf(BLD"Usage: clr -[r|o|y|g|b|m|c|p|w|k]"RST"\n");
        printf("  -r red    -o orange  -y yellow\n");
        printf("  -g green  -b blue    -m magenta\n");
        printf("  -c cyan   -p pink    -w white\n");
        printf("  -k reset to default (cyan)\n");
        return 1;
    }
    const char *a = c->argv[1];
    if (a[0] != '-' || a[1] == 0 || a[2] != 0) {
        printf(RED"clr: bad flag '%s'"RST"\n", a); return 1;
    }
    switch (a[1]) {
        case 'r': strcpy(g_theme,"\x1b[38;5;196m"); break;
        case 'o': strcpy(g_theme,"\x1b[38;5;208m"); break;
        case 'y': strcpy(g_theme,"\x1b[38;5;226m"); break;
        case 'g': strcpy(g_theme,"\x1b[38;5;82m");  break;
        case 'b': strcpy(g_theme,"\x1b[38;5;39m");  break;
        case 'm': strcpy(g_theme,"\x1b[38;5;207m"); break;
        case 'c': strcpy(g_theme,"\x1b[38;5;51m");  break;
        case 'p': strcpy(g_theme,"\x1b[38;5;213m"); break;
        case 'w': strcpy(g_theme,"\x1b[38;5;255m"); break;
        case 'k': strcpy(g_theme,"\x1b[38;5;51m");  break;
        default:
            printf(RED"clr: unknown colour '-%c'"RST"\n", a[1]); return 1;
    }
    printf("%sPrompt colour changed"RST"\n", g_theme);
    return 0;
}

typedef int (*BFn)(Cmd*);
typedef struct{const char *n;BFn fn;}BE;
static BE btab[]={
    {"[",b_test},{"alias",b_alias},{"cat",b_cat},{"cd",b_cd},{"chmod",b_chmod},
    {"clear",b_clear},{"cp",b_cp},{"date",b_date},{"df",b_df},{"du",b_du},
    {"echo",b_echo},{"edit",b_edit},{"nano",b_edit},{"vi",b_edit},{"snake",b_snake},{"tetris",b_tetris},{"pongy",b_pongy},{"chicken",b_chicken},{"menu",b_menu},{"files",b_files},{"web",b_web},{"clr",b_clr},{"calc",b_calc},{"figlet",b_figlet},{"ascii",b_figlet},{"poweroff",b_poweroff},{"shutdown",b_poweroff},{"reboot",b_reboot},
    {"env",b_env},{"false",b_false},{"fetch",b_fetch},{"file",b_file},
    {"find",b_find},{"free",b_free},{"grep",b_grep},{"head",b_head},{"help",b_help},
    {"history",b_history},{"hostname",b_hostname},{"id",b_id},{"kill",b_kill},
    {"ln",b_ln},{"ls",b_ls},{"mkdir",b_mkdir},{"mv",b_mv},{"ps",b_ps},{"pwd",b_pwd},
    {"read",b_read},{"rm",b_rm},{"rmdir",b_rmdir},{"sleep",b_sleep},{"source",b_source},
    {".  ",b_source},{"stat",b_stat},{"tail",b_tail},{"test",b_test},{"touch",b_touch},
    {"true",b_true},{"uname",b_uname},{"unalias",b_unalias},{"unset",b_unset},
    {"uptime",b_uptime},{"wc",b_wc},{"whoami",b_whoami},{NULL,NULL}};
static BFn find_builtin(const char *n){for(int i=0;btab[i].n;i++)if(strcmp(btab[i].n,n)==0)return btab[i].fn;return NULL;}

static int exec_single(Cmd *cmd,int in_fd,int out_fd){
    if(!cmd->argc||!cmd->argv[0])return 0;
    const char *name=cmd->argv[0];

    if(strcmp(name,"exit")==0){running=0;last_exit=cmd->argc>1?atoi(cmd->argv[1]):0;return last_exit;}
    if(strcmp(name,"eval")==0){char eb[SH_MAX_INPUT]="";for(int i=1;i<cmd->argc;i++){if(i>1)strcat(eb," ");strncat(eb,cmd->argv[i],SH_MAX_INPUT-strlen(eb)-1);}return run_line(eb);}
    if(strcmp(name,"exec")==0&&cmd->argc>1){execvp(cmd->argv[1],cmd->argv+1);perror("exec");return 1;}

    if(strchr(name,'=')&&name[0]!='='){char *eq=strchr(name,'=');char k[128];int kl=eq-name;memcpy(k,name,kl);k[kl]='\0';putenv(strdup(name));return 0;}

    BFn bfn=find_builtin(name);

    int need_fork=(in_fd!=0||out_fd!=1||cmd->in_file||cmd->out_file||cmd->err_file||cmd->err_to_out);
    if(bfn&&!need_fork){return bfn(cmd);}

    pid_t pid=fork();
    if(pid<0){perror("fork");return 1;}
    if(pid==0){

        if(in_fd!=0){dup2(in_fd,0);close(in_fd);}
        if(out_fd!=1){dup2(out_fd,1);close(out_fd);}
        if(cmd->in_file){int fd=open(cmd->in_file,O_RDONLY);if(fd<0){perror(cmd->in_file);exit(1);}dup2(fd,0);close(fd);}
        if(cmd->out_file){int fd=open(cmd->out_file,cmd->out_append?O_WRONLY|O_CREAT|O_APPEND:O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd<0){perror(cmd->out_file);exit(1);}dup2(fd,1);close(fd);}
        if(cmd->err_file){int fd=open(cmd->err_file,O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd<0){perror(cmd->err_file);exit(1);}dup2(fd,2);close(fd);}
        if(cmd->err_to_out)dup2(1,2);
        if(bfn){exit(bfn(cmd));}
        execvp(name,cmd->argv);
        fprintf(stderr,"%s%s%s: command not found\n",RED,name,RST);exit(127);}

    if(cmd->background){printf("[bg] %d\n",pid);return 0;}
    int status;waitpid(pid,&status,0);
    return WIFEXITED(status)?WEXITSTATUS(status):1;}

static int exec_pipeline(Cmd cmds[],int n){
    if(n==1)return exec_single(&cmds[0],0,1);
    int pipes[MAX_PIPES][2];
    for(int i=0;i<n-1;i++) pipe(pipes[i]);
    for(int i=0;i<n;i++){
        int in_fd =(i==0)?0:pipes[i-1][0];
        int out_fd=(i==n-1)?1:pipes[i][1];
        pid_t pid=fork();
        if(pid==0){
            if(in_fd!=0){dup2(in_fd,0);}
            if(out_fd!=1){dup2(out_fd,1);}

            for(int j=0;j<n-1;j++){close(pipes[j][0]);close(pipes[j][1]);}
            if(cmds[i].in_file){int fd=open(cmds[i].in_file,O_RDONLY);if(fd>=0){dup2(fd,0);close(fd);}}
            if(cmds[i].out_file){int fd=open(cmds[i].out_file,cmds[i].out_append?O_WRONLY|O_CREAT|O_APPEND:O_WRONLY|O_CREAT|O_TRUNC,0644);if(fd>=0){dup2(fd,1);close(fd);}}
            BFn bfn=find_builtin(cmds[i].argv[0]?cmds[i].argv[0]:"");
            if(bfn)exit(bfn(&cmds[i]));
            execvp(cmds[i].argv[0],cmds[i].argv);
            fprintf(stderr,"%s%s%s: command not found\n",RED,cmds[i].argv[0]?cmds[i].argv[0]:"?",RST);exit(127);}}
    for(int i=0;i<n-1;i++){close(pipes[i][0]);close(pipes[i][1]);}
    int status=0;for(int i=0;i<n;i++){int s;wait(&s);if(i==n-1&&WIFEXITED(s))status=WEXITSTATUS(s);}
    return status;}

static int b_kpanic(Cmd *c){(void)c;
    struct termios raw;
    tcgetattr(0,&raw);
    raw.c_lflag&=~(ECHO|ICANON);
    tcsetattr(0,TCSAFLUSH,&raw);
    /* red screen */
    int cols=80,rows=24;
    struct winsize ws;
    if(ioctl(1,TIOCGWINSZ,&ws)==0&&ws.ws_col&&ws.ws_row){cols=ws.ws_col;rows=ws.ws_row;}
    /* fill every cell with red background */
    write(1,"[2J[H[41m[97m[1m",21);
    for(int i=0;i<rows;i++){
        char mv[16]; snprintf(mv,16,"[%d;1H",i+1); write(1,mv,strlen(mv));
        for(int j=0;j<cols;j++) write(1," ",1);
    }
    write(1,"[H",3);
    /* centre the panic message */
    const char *lines[]={
        "[ TRIUMPH OS KERNEL PANIC ]",
        "",
        "Attempted to kill triumph.c",
        "You just tried to delete the OS itself.",
        "",
        "Fatal exception in core shell process",
        "RIP: 0000:ffffffffc0de1337",
        "RSP: 0000:ffff888003c3fe10",
        "RAX: 0000000000000000",
        "",
        "Call Trace:",
        "  b_rm+0x42/0x80",
        "  exec_single+0x1a3/0x2f0",
        "  run_line+0x88/0x100",
        "  main+0x4d/0x60",
        "",
        "---[ end Kernel panic - not syncing ]---",
        "",
        "Press any key to reboot...",
        NULL
    };
    int nlines=0; while(lines[nlines])nlines++;
    int oy=(rows-nlines)/2;
    for(int i=0;lines[i];i++){
        int llen=strlen(lines[i]);
        int ox=(cols-llen)/2; if(ox<0)ox=0;
        char mv[32]; snprintf(mv,32,"[%d;%dH",oy+i,ox);
        write(1,mv,strlen(mv));
        write(1,lines[i],llen);
    }
    fflush(stdout);
    /* flash effect */
    for(int f=0;f<6;f++){
        usleep(200000);
        write(1,f%2?"[41m":"[48;5;196m",f%2?5:11);
    }
    /* wait for keypress */
    unsigned char dummy; read(0,&dummy,1);
    /* reboot */
    write(1,"[0m[2J[H",12);
    sync();
    reboot(RB_AUTOBOOT);
    return 0;
}

static int run_line(char *line){

    char *cm=strchr(line,'#');

    if(cm&&(cm==line||*(cm-1)==' '||*(cm-1)=='\t'))*cm='\0';

    while(*line==' '||*line=='\t')line++;
    int len=strlen(line);while(len>0&&(line[len-1]==' '||line[len-1]=='\t'))line[--len]='\0';
    if(!*line)return 0;

    char expanded[SH_MAX_INPUT];strncpy(expanded,line,SH_MAX_INPUT-1);
    alias_expand(expanded,SH_MAX_INPUT);

    if((strstr(expanded,"kill")&&strstr(expanded,"triumph.c"))||
       (strstr(expanded,"rm")&&strstr(expanded,"triumph.c"))){
        Cmd dummy; memset(&dummy,0,sizeof(dummy));
        return b_kpanic(&dummy);
    }
    char *andand=strstr(expanded," && ");
    if(andand){char left[SH_MAX_INPUT];int ll=andand-expanded;memcpy(left,expanded,ll);left[ll]='\0';int r=run_line(left);if(r!=0)return r;return run_line(andand+4);}
    char *oror=strstr(expanded," || ");
    if(oror){char left[SH_MAX_INPUT];int ll=oror-expanded;memcpy(left,expanded,ll);left[ll]='\0';int r=run_line(left);if(r==0)return 0;return run_line(oror+4);}
    Cmd cmds[MAX_PIPES];
    int n=parse_cmds(expanded,cmds,MAX_PIPES);
    if(!n||!cmds[0].argc)return 0;
    int ret=exec_pipeline(cmds,n);
    last_exit=ret;return ret;}

static void show_banner(void){
    printf("\x1b[2J\x1b[H");
    printf(CYN BLD
    "  ████████╗██████╗ ██╗██╗   ██╗███╗   ███╗██████╗ ██╗  ██╗\n"
    "  ╚══██╔══╝██╔══██╗██║██║   ██║████╗ ████║██╔══██╗██║  ██║\n"
    "     ██║   ██████╔╝██║██║   ██║██╔████╔██║██████╔╝███████║\n"
    "     ██║   ██╔══██╗██║██║   ██║██║╚██╔╝██║██╔═══╝ ██╔══██║\n"
    "     ██║   ██║  ██║██║╚██████╔╝██║ ╚═╝ ██║██║     ██║  ██║\n"
    "     ╚═╝   ╚═╝  ╚═╝╚═╝ ╚═════╝ ╚═╝     ╚═╝╚═╝     ╚═╝  ╚═╝\n"
    RST GRY BLD
    "                  ██████╗ ███████╗\n"
    "                 ██╔═══██╗██╔════╝\n"
    "                 ██║   ██║███████╗\n"
    "                 ██║   ██║╚════██║\n"
    "                 ╚██████╔╝███████║\n"
    "                  ╚═════╝ ╚══════╝\n"
    RST);
    printf(GRY"  TTY-only live OS — boot from USB, install nothing.\n"RST);
    printf(GRY"  Type '"GRN"help"GRY"' for commands, '"GRN"fetch"GRY"' for system info.\n\n"RST);
}

static void setup_defaults(void){
    struct { const char *n,*v; } defs[]={
        {"ll","ls -lah"},{"la","ls -A"},
        {"..","cd .."},{"...","cd ../.."},
        {"cls","clear"},{"top","ps"},
        {"ports","cat /proc/net/tcp"},
        {NULL,NULL}};
    for(int i=0;defs[i].n;i++){
        aliases[alias_count].name=strdup(defs[i].n);
        aliases[alias_count].value=strdup(defs[i].v);
        alias_count++;}
    setenv("HOME","/root",0);
    setenv("PATH","/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin",0);
    setenv("TERM","xterm-256color",0);
    setenv("USER",getuid()==0?"root":"user",0);
    chdir(getenv("HOME"));}

int main(int argc,char *argv[]){

    if(argc>1&&argv[1][0]!='-'){
        Cmd dummy={.argc=2,.argv={argv[0],argv[1]}};
        return b_source(&dummy);}
    signals_init();
    setup_defaults();

    if(getpid()==1){
        system("mount -t proc  proc  /proc 2>/dev/null");
        system("mount -t sysfs sys   /sys  2>/dev/null");
        system("mount -t devtmpfs dev /dev  2>/dev/null");
        system("mount -t tmpfs  tmp  /tmp  2>/dev/null");}
    show_banner();
    /* Boot melody on PC speaker */
    pc_play(SND_BOOT);
    /* Boot directly into the fullscreen menu */
    { Cmd dc={0}; b_menu(&dc); }
    char line[SH_MAX_INPUT];
    while(running){
        print_prompt();
        int n=triumph_readline(line,SH_MAX_INPUT);
        if(n<0){printf("\n"BLD CYN"logout"RST"\n");break;}
        if(n==0)continue;
        hist_add(line);
        run_line(line);}
    if(getpid()==1){
        system("reboot -f 2>/dev/null");
        for(;;)pause();}
    return last_exit;}
