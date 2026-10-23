// AC Mod Loader - Assetto Corsa (PS4 build) mod loader payload
// Layout on console:
//   /data/ac_mods/mods.ini
//   /data/ac_mods/<modname>/content/cars/<carfolder>/...
//   /data/ac_mods/<modname>/content/tracks/<trackfolder>/...
// mods.ini:
//   [game]
//   titleid=CUSAxxxxx        ; set YOUR AC title id
//   [mods]
//   mymod=1                  ; 1 = on, 0 = off
// Web UI: http://localhost:9091 (open in console browser)

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

// --- game detection (same trick the Hitman payload uses: scan eboot.bin procs, read title id)
typedef struct {
    uint32_t app_id;
    uint32_t unknown1[2];
    uint32_t app_type;
    char     title_id[10];
    char     rest[0x200];
} app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

// --- on-screen notification
typedef struct { char pad[45]; char msg[3075]; } notify_req_t;
int sceKernelSendNotificationRequest(int, notify_req_t *, size_t, int);
static void notify(const char *text) {
    notify_req_t r; memset(&r, 0, sizeof r);
    strncpy(r.msg, text, sizeof(r.msg) - 1);
    sceKernelSendNotificationRequest(0, &r, sizeof r, 0);
}

#define MODS_ROOT "/data/ac_mods"
#define INI_PATH  MODS_ROOT "/mods.ini"
#define PORT      9091
#define MAX_MODS  64
#define MAX_MNT   512

typedef struct { char name[64]; int on; } mod_t;

static char   g_titleid[32] = "";
static mod_t  g_mods[MAX_MODS];
static int    g_nmods = 0;
static char   g_mounts[MAX_MNT][512];   // dst paths we mounted
static int    g_nmounts = 0;
static char   g_log[2048] = "";

static void logmsg(const char *m) {
    printf("[acml] %s\n", m);
    strncpy(g_log, m, sizeof(g_log) - 1);
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static mod_t *find_mod(const char *n) {
    for (int i = 0; i < g_nmods; i++)
        if (!strcmp(g_mods[i].name, n)) return &g_mods[i];
    return NULL;
}

static void load_ini(void) {
    FILE *f = fopen(INI_PATH, "r");
    char line[256], sect[32] = "";
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *s = strchr(line, ';'); if (s) *s = 0;
        s = line; while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s);
        while (l && (s[l-1] == '\n' || s[l-1] == '\r' || s[l-1] == ' ')) s[--l] = 0;
        if (!l) continue;
        if (s[0] == '[') { sscanf(s, "[%31[^]]", sect); continue; }
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0; char *k = s, *v = eq + 1;
        if (!strcmp(sect, "game") && !strcmp(k, "titleid")) {
            strncpy(g_titleid, v, sizeof(g_titleid) - 1);
        } else if (!strcmp(sect, "mods")) {
            mod_t *m = find_mod(k);
            if (m) m->on = atoi(v);
        }
    }
    fclose(f);
}

static void save_ini(void) {
    FILE *f = fopen(INI_PATH, "w");
    if (!f) { logmsg("cant write mods.ini"); return; }
    fprintf(f, "[game]\ntitleid=%s\n\n[mods]\n", g_titleid);
    for (int i = 0; i < g_nmods; i++)
        fprintf(f, "%s=%d\n", g_mods[i].name, g_mods[i].on);
    fclose(f);
}

static void scan_mods(void) {
    DIR *d = opendir(MODS_ROOT);
    struct dirent *e;
    g_nmods = 0;
    if (!d) return;
    while ((e = readdir(d)) && g_nmods < MAX_MODS) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s/content", MODS_ROOT, e->d_name);
        if (!is_dir(p)) continue;
        strncpy(g_mods[g_nmods].name, e->d_name, 63);
        g_mods[g_nmods].on = 0;
        g_nmods++;
    }
    closedir(d);
    load_ini();
}

static int mount_nullfs(const char *src, const char *dst) {
    struct iovec iov[6] = {
        { (void *)"fstype", 7 }, { (void *)"nullfs", 7 },
        { (void *)"fspath", 7 }, { (void *)dst, strlen(dst) + 1 },
        { (void *)"target", 7 }, { (void *)src, strlen(src) + 1 },
    };
    return nmount(iov, 6, 0);
}

// find the game's app0 root
static int game_root(char *out, size_t n) {
    const char *fmts[] = {
        "/mnt/sandbox/%s_000/app0",
        "/mnt/sandbox/pfsmnt/%s-app0",
        "/system_data/priv/mms/%s",
    };
    for (int i = 0; i < 3; i++) {
        snprintf(out, n, fmts[i], g_titleid);
        if (is_dir(out)) return 0;
    }
    return -1;
}

static void unmount_all(void) {
    for (int i = g_nmounts - 1; i >= 0; i--)
        unmount(g_mounts[i], MNT_FORCE);
    g_nmounts = 0;
}

// mount every <mod>/content/<kind>/<item> over game's content/<kind>/<item>
static void apply_mods(void) {
    char root[512];
    unmount_all();
    if (!g_titleid[0]) { logmsg("set titleid in mods.ini first"); return; }
    if (game_root(root, sizeof root)) { logmsg("game not running or path not found"); return; }

    int ok = 0, fail = 0;
    for (int i = 0; i < g_nmods; i++) {
        if (!g_mods[i].on) continue;
        const char *kinds[] = { "cars", "tracks" };
        for (int k = 0; k < 2; k++) {
            char kd[512]; DIR *d; struct dirent *e;
            snprintf(kd, sizeof kd, "%s/%s/content/%s", MODS_ROOT, g_mods[i].name, kinds[k]);
            if (!(d = opendir(kd))) continue;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                char src[512], dst[512];
                snprintf(src, sizeof src, "%s/%s", kd, e->d_name);
                snprintf(dst, sizeof dst, "%s/content/%s/%s", root, kinds[k], e->d_name);
                if (!is_dir(src)) continue;
                if (!is_dir(dst)) mkdir(dst, 0777);   // new items: may fail on read-only app0
                if (mount_nullfs(src, dst) == 0 && g_nmounts < MAX_MNT) {
                    strncpy(g_mounts[g_nmounts++], dst, 511); ok++;
                } else fail++;
            }
            closedir(d);
        }
    }
    snprintf(g_log, sizeof g_log, "applied: %d mounted, %d failed", ok, fail);
}


// ---- unionfs test: only touches /data/utest, never the game
static void filelog(const char *m) {
    FILE *f = fopen(MODS_ROOT "/log.txt", "a");
    if (f) { fprintf(f, "%s\n", m); fclose(f); }
}

static void touch_file(const char *path, const char *txt) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(txt, f); fclose(f); }
}

static void test_unionfs(void) {
    const char *lower = "/data/utest/lower", *upper = "/data/utest/upper";
    char m[256];
    mkdir("/data/utest", 0777); mkdir(lower, 0777); mkdir(upper, 0777);
    touch_file("/data/utest/lower/a.txt", "lower");
    touch_file("/data/utest/upper/b.txt", "upper");

    struct iovec iov[6] = {
        { (void *)"fstype", 7 }, { (void *)"unionfs", 8 },
        { (void *)"fspath", 7 }, { (void *)lower, strlen(lower) + 1 },
        { (void *)"from",   5 }, { (void *)upper, strlen(upper) + 1 },
    };
    if (nmount(iov, 6, 0) != 0) {
        snprintf(m, sizeof m, "unionfs: FAIL errno %d (%s)", errno, strerror(errno));
        logmsg(m); filelog(m); return;
    }
    int a = access("/data/utest/lower/a.txt", F_OK) == 0;
    int b = access("/data/utest/lower/b.txt", F_OK) == 0;
    struct statfs sf; memset(&sf, 0, sizeof sf);
    statfs(lower, &sf);
    snprintf(m, sizeof m, "unionfs: mounted, fstype=%s, sees a.txt=%d b.txt=%d -> %s",
             sf.f_fstypename, a, b, (a && b) ? "WORKS" : "mounted but NOT merging");
    unmount(lower, MNT_FORCE);
    logmsg(m); filelog(m);
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void apply_locked(void) {
    pthread_mutex_lock(&g_lock);
    apply_mods();
    pthread_mutex_unlock(&g_lock);
}

static void unmount_locked(void) {
    pthread_mutex_lock(&g_lock);
    unmount_all();
    pthread_mutex_unlock(&g_lock);
}

static pid_t find_game(void) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t size = 0;
    if (!g_titleid[0] || sysctl(mib, 4, NULL, &size, NULL, 0)) return -1;
    size += 32 * sizeof(struct kinfo_proc);
    char *buf = malloc(size);
    if (!buf) return -1;
    pid_t found = -1;
    if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
        for (char *p = buf; p < buf + size;) {
            struct kinfo_proc *ki = (struct kinfo_proc *)p;
            if (ki->ki_structsize <= 0) break;
            p += ki->ki_structsize;
            if (strcmp(ki->ki_comm, "eboot.bin")) continue;
            app_info_t info; memset(&info, 0, sizeof info);
            if (sceKernelGetAppInfo(ki->ki_pid, &info) == 0 &&
                !strncmp(info.title_id, g_titleid, 9)) { found = ki->ki_pid; break; }
        }
    }
    free(buf);
    return found;
}

// waits for the game to launch, then mounts enabled mods by itself
static void *watcher(void *arg) {
    pid_t handled = -1;
    (void)arg;
    for (;;) {
        pid_t pid = find_game();
        if (pid <= 0) {
            if (handled > 0) g_nmounts = 0;   // game closed, its sandbox mounts are gone
            handled = -1;
        } else if (pid != handled) {
            handled = pid;
            sleep(8);                          // let the game finish mounting app0
            apply_locked();
            notify(g_log);
        }
        sleep(1);
    }
    return NULL;
}

static void send_all(int c, const char *s) { send(c, s, strlen(s), 0); }

static void page(int c) {
    char buf[1024];
    send_all(c, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
        "<html><head><meta name=viewport content='width=device-width'>"
        "<style>body{background:#111;color:#eee;font:28px sans-serif;padding:30px}"
        "a{display:block;padding:18px;margin:10px 0;background:#222;color:#fff;"
        "text-decoration:none;border-radius:12px}.on{border-left:10px solid #2ecc71}"
        ".off{border-left:10px solid #e74c3c}</style></head><body><h1>AC Mod Loader</h1>");
    snprintf(buf, sizeof buf, "<p>titleid: %s<br>%s</p>", g_titleid[0] ? g_titleid : "NOT SET", g_log);
    send_all(c, buf);
    for (int i = 0; i < g_nmods; i++) {
        snprintf(buf, sizeof buf, "<a class=%s href='/toggle?m=%s'>%s - %s</a>",
                 g_mods[i].on ? "on" : "off", g_mods[i].name, g_mods[i].name,
                 g_mods[i].on ? "ON" : "OFF");
        send_all(c, buf);
    }
    send_all(c, "<a href='/apply'>APPLY MODS</a><a href='/unmount'>UNMOUNT ALL</a>"
                "<a href='/test'>TEST UNIONFS</a>"
                "<a href='/'>refresh</a></body></html>");
}

static void handle(int c) {
    char req[1024] = "";
    recv(c, req, sizeof req - 1, 0);
    if (!strncmp(req, "GET /toggle?m=", 14)) {
        char n[64] = ""; sscanf(req + 14, "%63[^ &]", n);
        mod_t *m = find_mod(n);
        if (m) { m->on = !m->on; save_ini(); }
    } else if (!strncmp(req, "GET /apply", 10)) {
        apply_locked();
    } else if (!strncmp(req, "GET /test", 9)) {
        test_unionfs();
    } else if (!strncmp(req, "GET /unmount", 12)) {
        unmount_locked(); logmsg("all mods unmounted");
    }
    page(c);
    close(c);
}

int main(void) {
    mkdir(MODS_ROOT, 0777);
    scan_mods();
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET; a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (void *)&a, sizeof a) || listen(s, 8)) { perror("bind"); return 1; }
    logmsg("ready");
    notify("AC Mod Loader ready - http://localhost:9091");
    pthread_t th;
    pthread_create(&th, NULL, watcher, NULL);
    pthread_detach(th);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c >= 0) handle(c);
    }
}
