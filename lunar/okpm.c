#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <stdarg.h>

#define OKPM_VERSION "3.0.0"
#define MAX_PKGS 512
#define MAX_DEPS 64
#define MAX_LINE 2048
#define MAX_NAME 128
#define MAX_VERSION 64
#define MAX_PATH 1024
#define MAX_URL 1024
#define MAX_HASH 65
#define MAX_DESC 256
#define MAX_GROUPS 32
#define MAX_HOOKS 64
#define MAX_HISTORY 1000
#define MAX_PROVIDES 64
#define MAX_VISITED 256
#define MAX_SNAPS 64
#define LOCK_TIMEOUT 10
#define LOG_MAX_SIZE (10 * 1024 * 1024)

#define STORE_DIR "/store"
#define DB_DIR "/var/lib/okpm"
#define DB_FILE "/var/lib/okpm/db"
#define LOCK_FILE "/var/lib/okpm/locks"
#define REPO_FILE "/var/lib/okpm/repos"
#define GROUP_FILE "/var/lib/okpm/groups"
#define HOOK_DIR "/var/lib/okpm/hooks"
#define HISTORY_FILE "/var/lib/okpm/history"
#define CACHE_DIR "/var/cache/okpm"
#define SNAP_DIR "/store/.snapshots"
#define CURRENT_LINK "/store/.current"
#define CONFIG_DIR "/etc/okpm"
#define CONFIG_FILE "/etc/okpm/okpm.conf"
#define LOG_FILE "/var/log/okpm.log"
#define PID_FILE "/var/run/okpm.pid"
#define TMP_BASE "/tmp"

#define SAFE_COPY(dst, src) do { \
    strncpy((dst), (src), sizeof(dst) - 1); \
    (dst)[sizeof(dst) - 1] = '\0'; \
} while (0)

#define SAFE_SNPRINTF(buf, ...) snprintf((buf), sizeof(buf), __VA_ARGS__)

typedef struct {
    char name[MAX_NAME];
    char version[MAX_VERSION];
    char deps[MAX_DEPS][MAX_NAME];
    int dep_count;
    char conflicts[MAX_DEPS][MAX_NAME];
    int conflict_count;
    char provides[MAX_PROVIDES][MAX_NAME];
    int provide_count;
    char replaces[MAX_DEPS][MAX_NAME];
    int replace_count;
    char arch[32];
    char source[MAX_URL];
    char desc[MAX_DESC];
    char groups[MAX_GROUPS][MAX_NAME];
    int group_count;
    char sha256[MAX_HASH];
    char install_path[MAX_PATH];
    int installed;
    int size;
} PkgEntry;

typedef struct {
    char url[MAX_URL];
    int priority;
    char type[16];
    char arch[32];
    int enabled;
} RepoSource;

typedef struct {
    char trigger[32];
    char exec[MAX_PATH];
    char when[16];
} Hook;

typedef struct {
    PkgEntry install_list[MAX_PKGS];
    int install_count;
    char remove_list[MAX_PKGS][MAX_NAME];
    int remove_count;
    char snapshot_id[MAX_NAME];
    int active;
} Transaction;

typedef struct {
    int yes;
    int verbose;
    int no_hooks;
    int no_verify;
    int download_only;
    int nodeps;
    int force;
} OkpmContext;

static OkpmContext g_ctx;
static int g_lock_fd = -1;
static char g_tmpdir[MAX_PATH];
static int g_config_loaded = 0;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    return s;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return st.st_size;
}

static void mkdir_p(const char *path) {
    char tmp[MAX_PATH];
    SAFE_SNPRINTF(tmp, "%s", path);
    int len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int has_tool(const char *tool) {
    char cmd[MAX_PATH];
    SAFE_SNPRINTF(cmd, "command -v '%s' >/dev/null 2>&1", tool);
    return system(cmd) == 0;
}

static int shell_escape(const char *input, char *out, int outlen) {
    int j = 0;
    out[j++] = '\'';
    for (int i = 0; input[i] && j < outlen - 3; i++) {
        if (input[i] == '\'') {
            if (j + 3 >= outlen - 1) break;
            out[j++] = '\'';
            out[j++] = '\\';
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            out[j++] = input[i];
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
    return 0;
}

static int safe_exec(const char **argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int safe_copy_file(const char *src, const char *dst) {
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) return -1;
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) { close(in_fd); return -1; }
    char buf[8192];
    ssize_t n;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(out_fd, buf + written, n - written);
            if (w < 0) { close(in_fd); close(out_fd); return -1; }
            written += w;
        }
    }
    close(in_fd);
    close(out_fd);
    return (n < 0) ? -1 : 0;
}

static int safe_remove_tree(const char *path) {
    if (!path || !*path || strcmp(path, "/") == 0) return -1;
    if (strlen(path) < 2) return -1;
    char esc[MAX_PATH * 2];
    shell_escape(path, esc, sizeof(esc));
    char cmd[MAX_PATH * 3];
    SAFE_SNPRINTF(cmd, "rm -rf %s 2>/dev/null", esc);
    return system(cmd);
}

static void rotate_log(void) {
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > LOG_MAX_SIZE) {
        char old[MAX_PATH];
        SAFE_SNPRINTF(old, "%s.old", LOG_FILE);
        rename(LOG_FILE, old);
    }
}

static void log_msg(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    rotate_log();
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        vfprintf(f, fmt, args);
        fprintf(f, "\n");
        fclose(f);
    }
    va_end(args);
}

static void log_write(const char *action, const char *name, const char *version, int success) {
    FILE *f = fopen(HISTORY_FILE, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    fprintf(f, "%s %s %s %04d-%02d-%02d %02d:%02d:%02d %s\n",
            action, name, version ? version : "",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec,
            success ? "ok" : "fail");
    fclose(f);
    log_msg("%s %s %s: %s", action, name, version ? version : "", success ? "ok" : "fail");
}

static int acquire_lock(void) {
    g_lock_fd = open(PID_FILE, O_CREAT | O_RDWR, 0644);
    if (g_lock_fd < 0) return -1;
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "Error: okpm is already running (or lock held)\n");
        close(g_lock_fd);
        g_lock_fd = -1;
        return -1;
    }
    char pid_buf[16];
    SAFE_SNPRINTF(pid_buf, "%d\n", (int)getpid());
    write(g_lock_fd, pid_buf, strlen(pid_buf));
    return 0;
}

static void release_lock(void) {
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
        unlink(PID_FILE);
    }
}

static void cleanup_tmpdir(void) {
    if (g_tmpdir[0]) safe_remove_tree(g_tmpdir);
}

static int init_tmpdir(void) {
    SAFE_SNPRINTF(g_tmpdir, "%s/okpm_%d", TMP_BASE, (int)getpid());
    mkdir_p(g_tmpdir);
    atexit(cleanup_tmpdir);
    return 0;
}

static void parse_list_item(char *line, char deps[][MAX_NAME], int *count, int max) {
    char *p = strchr(line, '-');
    if (!p) return;
    p++;
    char *item = trim(p);
    if (*item && *count < max) {
        SAFE_COPY(deps[*count], item);
        (*count)++;
    }
}

static int parse_meta(const char *yaml_path, PkgEntry *pkg) {
    FILE *f = fopen(yaml_path, "r");
    if (!f) return -1;
    memset(pkg, 0, sizeof(PkgEntry));
    char line[MAX_LINE];
    char section[32] = "";
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (*trimmed == '\0' || *trimmed == '#') continue;
        if (*trimmed == '-' && strlen(section) > 0) {
            if (strcmp(section, "deps") == 0)
                parse_list_item(trimmed, pkg->deps, &pkg->dep_count, MAX_DEPS);
            else if (strcmp(section, "conflicts") == 0)
                parse_list_item(trimmed, pkg->conflicts, &pkg->conflict_count, MAX_DEPS);
            else if (strcmp(section, "provides") == 0)
                parse_list_item(trimmed, pkg->provides, &pkg->provide_count, MAX_PROVIDES);
            else if (strcmp(section, "replaces") == 0)
                parse_list_item(trimmed, pkg->replaces, &pkg->replace_count, MAX_DEPS);
            else if (strcmp(section, "groups") == 0)
                parse_list_item(trimmed, pkg->groups, &pkg->group_count, MAX_GROUPS);
            continue;
        }
        char *colon = strchr(trimmed, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = trim(trimmed);
        char *val = trim(colon + 1);
        if (*val == '\0') {
            SAFE_COPY(section, key);
            continue;
        }
        section[0] = '\0';
        if (strcmp(key, "name") == 0) SAFE_COPY(pkg->name, val);
        else if (strcmp(key, "version") == 0) SAFE_COPY(pkg->version, val);
        else if (strcmp(key, "arch") == 0) SAFE_COPY(pkg->arch, val);
        else if (strcmp(key, "source") == 0) SAFE_COPY(pkg->source, val);
        else if (strcmp(key, "desc") == 0 || strcmp(key, "description") == 0)
            SAFE_COPY(pkg->desc, val);
        else if (strcmp(key, "sha256") == 0) SAFE_COPY(pkg->sha256, val);
        else if (strcmp(key, "size") == 0) pkg->size = atoi(val);
    }
    fclose(f);
    return 0;
}

static void load_config(void) {
    if (g_config_loaded) return;
    g_config_loaded = 1;
    if (!file_exists(CONFIG_FILE)) return;
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (*trimmed == '\0' || *trimmed == '#') continue;
        char *eq = strchr(trimmed, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(trimmed);
        char *val = trim(eq + 1);
        if (strcmp(key, "no_verify") == 0) g_ctx.no_verify = atoi(val);
        else if (strcmp(key, "no_hooks") == 0) g_ctx.no_hooks = atoi(val);
        else if (strcmp(key, "download_only") == 0) g_ctx.download_only = atoi(val);
    }
    fclose(f);
}

static void compute_sha256(const char *path, char *out) {
    char esc[MAX_PATH * 2];
    shell_escape(path, esc, sizeof(esc));
    char cmd[MAX_PATH * 3];
    SAFE_SNPRINTF(cmd, "sha256sum %s 2>/dev/null | awk '{print $1}'", esc);
    FILE *p = popen(cmd, "r");
    if (!p) { out[0] = '\0'; return; }
    if (fgets(out, MAX_HASH, p)) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = '\0';
    } else out[0] = '\0';
    pclose(p);
}

static int verify_sha256(const char *path, const char *expected) {
    if (g_ctx.no_verify || !expected[0]) return 0;
    char actual[MAX_HASH];
    compute_sha256(path, actual);
    if (actual[0] && strcmp(actual, expected) != 0) return -1;
    return 0;
}

static int verify_signature(const char *pkg_file) {
    if (g_ctx.no_verify) return 0;
    char sig_file[MAX_PATH];
    SAFE_SNPRINTF(sig_file, "%s.sig", pkg_file);
    if (!file_exists(sig_file)) return 0;
    char esc_pkg[MAX_PATH * 2], esc_sig[MAX_PATH * 2];
    shell_escape(pkg_file, esc_pkg, sizeof(esc_pkg));
    shell_escape(sig_file, esc_sig, sizeof(esc_sig));
    char cmd[MAX_PATH * 4];
    SAFE_SNPRINTF(cmd, "gpg --verify %s %s 2>/dev/null", esc_sig, esc_pkg);
    return system(cmd) == 0 ? 0 : -1;
}

static void db_init(void) {
    mkdir_p(DB_DIR);
    mkdir_p(STORE_DIR);
    mkdir_p(SNAP_DIR);
    mkdir_p(CACHE_DIR);
    mkdir_p(HOOK_DIR);
    mkdir_p(CONFIG_DIR);
    init_tmpdir();
    if (!file_exists(DB_FILE)) { FILE *f = fopen(DB_FILE, "w"); if (f) fclose(f); }
    if (!file_exists(LOCK_FILE)) { FILE *f = fopen(LOCK_FILE, "w"); if (f) fclose(f); }
    if (!file_exists(REPO_FILE)) { FILE *f = fopen(REPO_FILE, "w"); if (f) fclose(f); }
    if (!file_exists(GROUP_FILE)) { FILE *f = fopen(GROUP_FILE, "w"); if (f) fclose(f); }
    if (!file_exists(HISTORY_FILE)) { FILE *f = fopen(HISTORY_FILE, "w"); if (f) fclose(f); }
    if (!file_exists(CURRENT_LINK) && !dir_exists(CURRENT_LINK)) symlink("/", CURRENT_LINK);
}

static int db_is_installed(const char *name) {
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char db_name[MAX_NAME];
        if (sscanf(line, "%127s", db_name) == 1 && strcmp(db_name, name) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int db_provides_installed(const char *dep) {
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    int found = 0;
    while (fgets(line, sizeof(line), f) && !found) {
        char db_name[MAX_NAME], db_version[MAX_VERSION], db_path[MAX_PATH];
        if (sscanf(line, "%127s %63s %511s", db_name, db_version, db_path) != 3) continue;
        if (strcmp(db_name, dep) == 0) { found = 1; break; }
        char meta_path[MAX_PATH];
        SAFE_SNPRINTF(meta_path, "%s/meta.yaml", db_path);
        if (!file_exists(meta_path)) continue;
        PkgEntry pkg;
        if (parse_meta(meta_path, &pkg) != 0) continue;
        for (int i = 0; i < pkg.provide_count; i++) {
            if (strcmp(pkg.provides[i], dep) == 0) { found = 1; break; }
        }
    }
    fclose(f);
    return found;
}

static int db_get_entry(const char *name, PkgEntry *pkg) {
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char db_name[MAX_NAME], db_version[MAX_VERSION], db_path[MAX_PATH];
        if (sscanf(line, "%127s %63s %511s", db_name, db_version, db_path) == 3 &&
            strcmp(db_name, name) == 0) {
            memset(pkg, 0, sizeof(PkgEntry));
            SAFE_COPY(pkg->name, db_name);
            SAFE_COPY(pkg->version, db_version);
            SAFE_COPY(pkg->install_path, db_path);
            pkg->installed = 1;
            char meta_path[MAX_PATH];
            SAFE_SNPRINTF(meta_path, "%s/meta.yaml", db_path);
            if (file_exists(meta_path)) parse_meta(meta_path, pkg);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

static int db_add(const PkgEntry *pkg) {
    FILE *f = fopen(DB_FILE, "a");
    if (!f) return -1;
    fprintf(f, "%s %s %s\n", pkg->name, pkg->version, pkg->install_path);
    fclose(f);
    return 0;
}

static int db_remove(const char *name) {
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return -1;
    char tmp_path[MAX_PATH];
    SAFE_SNPRINTF(tmp_path, "%s/db_tmp", g_tmpdir);
    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) { fclose(f); return -1; }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char db_name[MAX_NAME];
        if (sscanf(line, "%127s", db_name) == 1 && strcmp(db_name, name) != 0)
            fputs(line, tmp);
    }
    fclose(f);
    fclose(tmp);
    rename(tmp_path, DB_FILE);
    return 0;
}

static int db_count(void) {
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return 0;
    int count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char name[MAX_NAME];
        if (sscanf(line, "%127s", name) == 1) count++;
    }
    fclose(f);
    return count;
}

static long db_total_size(void) {
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return 0;
    long total = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char name[MAX_NAME], ver[MAX_VERSION], path[MAX_PATH];
        if (sscanf(line, "%127s %63s %511s", name, ver, path) == 3) {
            char esc[MAX_PATH * 2];
            shell_escape(path, esc, sizeof(esc));
            char cmd[MAX_PATH * 3];
            SAFE_SNPRINTF(cmd, "du -sb %s 2>/dev/null | awk '{print $1}'", esc);
            FILE *p = popen(cmd, "r");
            if (p) {
                char buf[32];
                if (fgets(buf, sizeof(buf), p)) total += atol(buf);
                pclose(p);
            }
        }
    }
    fclose(f);
    return total;
}

static int is_locked(const char *name) {
    FILE *f = fopen(LOCK_FILE, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char lock_name[MAX_NAME];
        if (sscanf(line, "%127s", lock_name) == 1 && strcmp(lock_name, name) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int lock_add(const char *name, const char *version) {
    if (is_locked(name)) return 0;
    FILE *f = fopen(LOCK_FILE, "a");
    if (!f) return -1;
    fprintf(f, "%s %s\n", name, version);
    fclose(f);
    return 0;
}

static int lock_remove(const char *name) {
    FILE *f = fopen(LOCK_FILE, "r");
    if (!f) return -1;
    char tmp_path[MAX_PATH];
    SAFE_SNPRINTF(tmp_path, "%s/lock_tmp", g_tmpdir);
    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) { fclose(f); return -1; }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char lock_name[MAX_NAME];
        if (sscanf(line, "%127s", lock_name) == 1 && strcmp(lock_name, name) != 0)
            fputs(line, tmp);
    }
    fclose(f);
    fclose(tmp);
    rename(tmp_path, LOCK_FILE);
    return 0;
}

static int repo_add(const char *url, int priority, const char *type, const char *arch) {
    FILE *f = fopen(REPO_FILE, "a");
    if (!f) return -1;
    fprintf(f, "%s %d %s %s %d\n", url, priority, type, arch ? arch : "any", 1);
    fclose(f);
    return 0;
}

static int repo_count(void) {
    FILE *f = fopen(REPO_FILE, "r");
    if (!f) return 0;
    int count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) count++;
    fclose(f);
    return count;
}

static int repo_remove(const char *url) {
    FILE *f = fopen(REPO_FILE, "r");
    if (!f) return -1;
    char tmp_path[MAX_PATH];
    SAFE_SNPRINTF(tmp_path, "%s/repo_tmp", g_tmpdir);
    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) { fclose(f); return -1; }
    char line[MAX_LINE];
    int removed = 0;
    while (fgets(line, sizeof(line), f)) {
        char repo_url[MAX_URL];
        sscanf(line, "%1023s", repo_url);
        if (strcmp(repo_url, url) != 0) fputs(line, tmp);
        else removed = 1;
    }
    fclose(f);
    fclose(tmp);
    rename(tmp_path, REPO_FILE);
    return removed ? 0 : -1;
}

static int group_add(const char *name, const char *desc) {
    FILE *f = fopen(GROUP_FILE, "a");
    if (!f) return -1;
    fprintf(f, "%s %s\n", name, desc ? desc : "");
    fclose(f);
    return 0;
}

static int group_add_member(const char *group, const char *pkg) {
    char path[MAX_PATH];
    SAFE_SNPRINTF(path, "%s/%s.members", DB_DIR, group);
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    fprintf(f, "%s\n", pkg);
    fclose(f);
    return 0;
}

static int group_list_members(const char *group) {
    char path[MAX_PATH];
    SAFE_SNPRINTF(path, "%s/%s.members", DB_DIR, group);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char *m = trim(line);
        if (*m) { printf("  %s\n", m); count++; }
    }
    fclose(f);
    if (count == 0) printf("  (empty group)\n");
    return 0;
}

static int hook_load_all(Hook *hooks, int *count) {
    *count = 0;
    DIR *d = opendir(HOOK_DIR);
    if (!d) return -1;
    struct dirent *entry;
    while ((entry = readdir(d)) && *count < MAX_HOOKS) {
        if (entry->d_name[0] == '.') continue;
        char path[MAX_PATH];
        SAFE_SNPRINTF(path, "%s/%s", HOOK_DIR, entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        Hook *h = &hooks[*count];
        memset(h, 0, sizeof(Hook));
        SAFE_COPY(h->trigger, entry->d_name);
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            char *trimmed = trim(line);
            if (*trimmed == '\0' || *trimmed == '#') continue;
            char *eq = strchr(trimmed, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = trim(trimmed);
            char *val = trim(eq + 1);
            if (strcmp(key, "exec") == 0) SAFE_COPY(h->exec, val);
            else if (strcmp(key, "when") == 0) SAFE_COPY(h->when, val);
        }
        fclose(f);
        if (h->exec[0]) (*count)++;
    }
    closedir(d);
    return 0;
}

static int hook_run(const char *trigger, const char *when, const char *pkg_name) {
    if (g_ctx.no_hooks) return 0;
    Hook hooks[MAX_HOOKS];
    int count;
    hook_load_all(hooks, &count);
    for (int i = 0; i < count; i++) {
        if (strstr(hooks[i].trigger, trigger) && strcmp(hooks[i].when, when) == 0) {
            char esc_name[MAX_NAME * 2];
            shell_escape(pkg_name, esc_name, sizeof(esc_name));
            char cmd[MAX_PATH + MAX_NAME * 2 + 32];
            SAFE_SNPRINTF(cmd, "%s %s 2>/dev/null", hooks[i].exec, esc_name);
            if (g_ctx.verbose) printf("  hook: %s\n", cmd);
            system(cmd);
        }
    }
    return 0;
}

static int extract_okra(const char *pkg_file, const char *dest) {
    mkdir_p(dest);
    char esc_pkg[MAX_PATH * 2], esc_dest[MAX_PATH * 2];
    shell_escape(pkg_file, esc_pkg, sizeof(esc_pkg));
    shell_escape(dest, esc_dest, sizeof(esc_dest));
    char dec_tar[MAX_PATH];
    SAFE_SNPRINTF(dec_tar, "%s/dec.tar", g_tmpdir);
    char esc_dec[MAX_PATH * 2];
    shell_escape(dec_tar, esc_dec, sizeof(esc_dec));
    char cmd[MAX_PATH * 8];
    SAFE_SNPRINTF(cmd,
        "tar --zstd -xf %s -C %s 2>/dev/null || "
        "(zstd -d %s -o %s 2>/dev/null && tar -xf %s -C %s 2>/dev/null) || "
        "tar -xf %s -C %s 2>/dev/null || "
        "tar -xjf %s -C %s 2>/dev/null || "
        "tar -xJf %s -C %s 2>/dev/null || "
        "unzip -o %s -d %s 2>/dev/null",
        esc_pkg, esc_dest, esc_pkg, esc_dec, esc_dec, esc_dest,
        esc_pkg, esc_dest, esc_pkg, esc_dest, esc_pkg, esc_dest, esc_pkg, esc_dest);
    return system(cmd);
}

static int run_script(const char *script_path) {
    if (!file_exists(script_path)) return 0;
    char esc[MAX_PATH * 2];
    shell_escape(script_path, esc, sizeof(esc));
    char cmd[MAX_PATH * 3];
    SAFE_SNPRINTF(cmd, "chmod +x %s 2>/dev/null && %s 2>&1", esc, esc);
    return system(cmd);
}

static int find_meta_in_dir(const char *dir, char *out_meta, char *out_base) {
    char meta_path[MAX_PATH];
    SAFE_SNPRINTF(meta_path, "%s/meta.yaml", dir);
    if (file_exists(meta_path)) {
        SAFE_COPY(out_meta, meta_path);
        out_base[0] = '\0';
        return 0;
    }
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *entry;
    int found = -1;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        size_t name_len = strlen(entry->d_name);
        if (name_len >= MAX_PATH) continue;
        SAFE_SNPRINTF(meta_path, "%s/%s/meta.yaml", dir, entry->d_name);
        if (file_exists(meta_path)) {
            SAFE_COPY(out_meta, meta_path);
            SAFE_COPY(out_base, entry->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

static int check_file_conflicts(const char *install_dir) {
    char bin_dir[MAX_PATH];
    SAFE_SNPRINTF(bin_dir, "%s/usr/bin", install_dir);
    if (!dir_exists(bin_dir)) return 0;
    DIR *bd = opendir(bin_dir);
    if (!bd) return 0;
    struct dirent *entry;
    int conflicts = 0;
    while ((entry = readdir(bd))) {
        if (entry->d_name[0] == '.') continue;
        char link_path[MAX_PATH];
        SAFE_SNPRINTF(link_path, "/usr/bin/%s", entry->d_name);
        if (file_exists(link_path)) {
            struct stat st;
            if (lstat(link_path, &st) == 0 && !S_ISLNK(st.st_mode)) {
                fprintf(stderr, "  Warning: /usr/bin/%s already exists (non-symlink)\n", entry->d_name);
                conflicts++;
            }
        }
    }
    closedir(bd);
    return conflicts;
}

static int check_conflicts(const PkgEntry *pkg) {
    for (int i = 0; i < pkg->conflict_count; i++) {
        if (db_is_installed(pkg->conflicts[i]) || db_provides_installed(pkg->conflicts[i])) {
            fprintf(stderr, "Error: conflicts with installed package %s\n", pkg->conflicts[i]);
            return -1;
        }
    }
    for (int i = 0; i < pkg->replace_count; i++) {
        if (db_is_installed(pkg->replaces[i])) {
            printf("  replacing %s\n", pkg->replaces[i]);
            remove_package(pkg->replaces[i]);
        }
    }
    return 0;
}

static void create_symlinks(const char *install_dir, const char *subdir, const char *target) {
    char dir[MAX_PATH];
    SAFE_SNPRINTF(dir, "%s/%s", install_dir, subdir);
    if (!dir_exists(dir)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        size_t name_len = strlen(entry->d_name);
        if (name_len >= MAX_NAME) continue;
        char src[MAX_PATH], dst[MAX_PATH];
        SAFE_SNPRINTF(src, "%s/%s", dir, entry->d_name);
        SAFE_SNPRINTF(dst, "%s/%s", target, entry->d_name);
        struct stat st;
        if (lstat(dst, &st) == 0) {
            if (S_ISLNK(st.st_mode)) unlink(dst);
            else if (S_ISDIR(st.st_mode)) continue;
            else {
                if (!g_ctx.force) {
                    fprintf(stderr, "  Warning: skipping %s (exists, use --force)\n", dst);
                    continue;
                }
                unlink(dst);
            }
        }
        symlink(src, dst);
        if (g_ctx.verbose) printf("  linked %s -> %s\n", dst, src);
    }
    closedir(d);
}

static void remove_symlinks(const char *install_dir, const char *subdir, const char *target) {
    char dir[MAX_PATH];
    SAFE_SNPRINTF(dir, "%s/%s", install_dir, subdir);
    if (!dir_exists(dir)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        size_t name_len = strlen(entry->d_name);
        if (name_len >= MAX_NAME) continue;
        char link_path[MAX_PATH];
        SAFE_SNPRINTF(link_path, "%s/%s", target, entry->d_name);
        struct stat st;
        if (lstat(link_path, &st) == 0 && S_ISLNK(st.st_mode)) {
            char expected[MAX_PATH];
            SAFE_SNPRINTF(expected, "%s/%s/%s", install_dir, subdir, entry->d_name);
            char actual[MAX_PATH];
            int n = readlink(link_path, actual, sizeof(actual) - 1);
            if (n > 0) { actual[n] = '\0'; if (strcmp(actual, expected) == 0) unlink(link_path); }
        }
    }
    closedir(d);
}

static int find_okra_local(const char *name, char *out) {
    const char *dirs[] = {".", CACHE_DIR, "/store", NULL};
    for (int i = 0; dirs[i]; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *entry;
        while ((entry = readdir(d))) {
            if (strstr(entry->d_name, ".okra") && strstr(entry->d_name, name)) {
                SAFE_SNPRINTF(out, "%s/%s", dirs[i], entry->d_name);
                closedir(d);
                return 0;
            }
        }
        closedir(d);
    }
    return -1;
}

static int download_package(const char *name, char *out_file) {
    if (find_okra_local(name, out_file) == 0) {
        if (g_ctx.verbose) printf("  found local: %s\n", out_file);
        return 0;
    }
    FILE *f = fopen(REPO_FILE, "r");
    if (!f) return -1;
    char repos[16][MAX_LINE];
    int repo_idx[16];
    int rc = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f) && rc < 16) {
        char url[MAX_URL], type[16], arch[32];
        int pri, enabled;
        if (sscanf(line, "%1023s %d %15s %31s %d", url, &pri, type, arch, &enabled) >= 4) {
            if (enabled == 0) continue;
            SAFE_COPY(repos[rc], line);
            repo_idx[rc] = pri;
            rc++;
        }
    }
    fclose(f);
    for (int i = 0; i < rc - 1; i++) {
        for (int j = i + 1; j < rc; j++) {
            if (repo_idx[j] < repo_idx[i]) {
                int tmp = repo_idx[i]; repo_idx[i] = repo_idx[j]; repo_idx[j] = tmp;
                char tmp2[MAX_LINE]; strcpy(tmp2, repos[i]); strcpy(repos[i], repos[j]); strcpy(repos[j], tmp2);
            }
        }
    }
    for (int i = 0; i < rc; i++) {
        char url[MAX_URL], type[16], arch[32];
        int pri, enabled;
        sscanf(repos[i], "%1023s %d %15s %31s %d", url, &pri, type, arch, &enabled);
        char full_url[MAX_URL * 2];
        SAFE_SNPRINTF(full_url, "%s/%s.okra", url, name);
        char cached_file[MAX_PATH];
        SAFE_SNPRINTF(cached_file, "%s/%s.okra", CACHE_DIR, name);
        char esc_url[MAX_URL * 2], esc_cache[MAX_PATH * 2];
        shell_escape(full_url, esc_url, sizeof(esc_url));
        shell_escape(cached_file, esc_cache, sizeof(esc_cache));
        char cmd[MAX_URL * 3 + MAX_PATH * 3];
        SAFE_SNPRINTF(cmd,
            "wget -q --tries=3 --timeout=30 -c %s -O %s 2>/dev/null || "
            "curl -sL --retry 3 --connect-timeout 30 %s -o %s 2>/dev/null",
            esc_url, esc_cache, esc_url, esc_cache);
        if (system(cmd) == 0 && file_exists(cached_file) && file_size(cached_file) > 0) {
            SAFE_COPY(out_file, cached_file);
            printf("  downloaded %s (%ld bytes)\n", name, file_size(cached_file));
            return 0;
        }
        unlink(cached_file);
    }
    return -1;
}

static int has_visited(const char *visited[][MAX_NAME], int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(visited[i], name) == 0) return 1;
    }
    return 0;
}

static int resolve_deps(const char *name, int depth, int *resolved_count,
                        const char *visited[], int *visited_count) {
    if (depth > 20) {
        fprintf(stderr, "Error: dependency depth exceeded for %s\n", name);
        return -1;
    }
    if (has_visited(visited, *visited_count, name)) {
        fprintf(stderr, "Error: circular dependency detected: %s\n", name);
        return -1;
    }
    if (*visited_count >= MAX_VISITED) {
        fprintf(stderr, "Error: too many dependencies\n");
        return -1;
    }
    SAFE_COPY((char *)visited[*visited_count], name);
    (*visited_count)++;
    if (db_is_installed(name) || db_provides_installed(name)) return 0;
    char pkg_file[MAX_PATH];
    if (download_package(name, pkg_file) != 0) {
        fprintf(stderr, "Error: cannot find dependency %s\n", name);
        return -1;
    }
    char tmp_extract[MAX_PATH];
    SAFE_SNPRINTF(tmp_extract, "%s/dep_%d_%d", g_tmpdir, depth, (int)getpid());
    if (extract_okra(pkg_file, tmp_extract) != 0) return -1;
    char meta_path[MAX_PATH];
    char base[MAX_PATH];
    int ret = -1;
    if (find_meta_in_dir(tmp_extract, meta_path, base) != 0) goto cleanup;
    {
        PkgEntry pkg;
        if (parse_meta(meta_path, &pkg) != 0) goto cleanup;
        for (int i = 0; i < pkg.dep_count; i++) {
            if (!db_is_installed(pkg.deps[i]) && !db_provides_installed(pkg.deps[i])) {
                printf("  resolving dependency: %s (depth %d)\n", pkg.deps[i], depth);
                if (resolve_deps(pkg.deps[i], depth + 1, resolved_count, visited, visited_count) != 0) {
                    if (!g_ctx.force) goto cleanup;
                }
            }
        }
        printf("  installing dependency: %s\n", name);
        (*resolved_count)++;
        ret = install_package(pkg_file);
    }
cleanup:
    safe_remove_tree(tmp_extract);
    return ret;
}

static char *create_snapshot(void) {
    static char snap_id[MAX_NAME];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    SAFE_SNPRINTF(snap_id, "snap_%04d%02d%02d%02d%02d%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    char snap_path[MAX_PATH];
    SAFE_SNPRINTF(snap_path, "%s/%s", SNAP_DIR, snap_id);
    mkdir_p(snap_path);
    char dst_db[MAX_PATH]; SAFE_SNPRINTF(dst_db, "%s/db", snap_path);
    char dst_lock[MAX_PATH]; SAFE_SNPRINTF(dst_lock, "%s/locks", snap_path);
    char dst_group[MAX_PATH]; SAFE_SNPRINTF(dst_group, "%s/groups", snap_path);
    safe_copy_file(DB_FILE, dst_db);
    safe_copy_file(LOCK_FILE, dst_lock);
    safe_copy_file(GROUP_FILE, dst_group);
    DIR *d = opendir(DB_DIR);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d))) {
            if (strstr(entry->d_name, ".members") == NULL) continue;
            char src[MAX_PATH], dst[MAX_PATH];
            SAFE_SNPRINTF(src, "%s/%s", DB_DIR, entry->d_name);
            SAFE_SNPRINTF(dst, "%s/%s", snap_path, entry->d_name);
            safe_copy_file(src, dst);
        }
        closedir(d);
    }
    return snap_id;
}

static int switch_snapshot(const char *snap_id) {
    char snap_path[MAX_PATH];
    SAFE_SNPRINTF(snap_path, "%s/%s", SNAP_DIR, snap_id);
    if (!dir_exists(snap_path)) return -1;
    unlink(CURRENT_LINK);
    symlink(snap_path, CURRENT_LINK);
    return 0;
}

static int count_snapshots(void) {
    DIR *d = opendir(SNAP_DIR);
    if (!d) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        count++;
    }
    closedir(d);
    return count;
}

static int cleanup_old_snapshots(int keep) {
    DIR *d = opendir(SNAP_DIR);
    if (!d) return 0;
    char snaps[MAX_SNAPS][MAX_NAME];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) && count < MAX_SNAPS) {
        if (entry->d_name[0] == '.') continue;
        SAFE_COPY(snaps[count], entry->d_name);
        count++;
    }
    closedir(d);
    if (count <= keep) return 0;
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(snaps[i], snaps[j]) > 0) {
                char tmp[MAX_NAME];
                strcpy(tmp, snaps[i]); strcpy(snaps[i], snaps[j]); strcpy(snaps[j], tmp);
            }
        }
    }
    int to_remove = count - keep;
    for (int i = 0; i < to_remove; i++) {
        char path[MAX_PATH];
        SAFE_SNPRINTF(path, "%s/%s", SNAP_DIR, snaps[i]);
        safe_remove_tree(path);
        if (g_ctx.verbose) printf("  cleaned old snapshot: %s\n", snaps[i]);
    }
    return to_remove;
}

static int rollback_snapshot(void) {
    DIR *d = opendir(SNAP_DIR);
    if (!d) {
        fprintf(stderr, "Error: no snapshots available\n");
        return -1;
    }
    char snaps[MAX_SNAPS][MAX_NAME];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) && count < MAX_SNAPS) {
        if (entry->d_name[0] == '.') continue;
        SAFE_COPY(snaps[count], entry->d_name);
        count++;
    }
    closedir(d);
    if (count < 2) {
        fprintf(stderr, "Error: no previous snapshot to rollback to\n");
        return -1;
    }
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(snaps[i], snaps[j]) < 0) {
                char tmp[MAX_NAME];
                strcpy(tmp, snaps[i]); strcpy(snaps[i], snaps[j]); strcpy(snaps[j], tmp);
            }
        }
    }
    char *prev_snap = snaps[count - 2];
    printf("OkraLinux: rolling back to %s\n", prev_snap);
    char snap_db[MAX_PATH], snap_lock[MAX_PATH], snap_group[MAX_PATH];
    SAFE_SNPRINTF(snap_db, "%s/%s/db", SNAP_DIR, prev_snap);
    SAFE_SNPRINTF(snap_lock, "%s/%s/locks", SNAP_DIR, prev_snap);
    SAFE_SNPRINTF(snap_group, "%s/%s/groups", SNAP_DIR, prev_snap);
    if (file_exists(snap_db)) safe_copy_file(snap_db, DB_FILE);
    if (file_exists(snap_lock)) safe_copy_file(snap_lock, LOCK_FILE);
    if (file_exists(snap_group)) safe_copy_file(snap_group, GROUP_FILE);
    switch_snapshot(prev_snap);
    printf("OkraLinux: rollback complete\n");
    log_write("rollback", prev_snap, "", 1);
    return 0;
}

static int confirm_action(const char *prompt) {
    if (g_ctx.yes) return 1;
    printf("%s [y/N] ", prompt);
    fflush(stdout);
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    return buf[0] == 'y' || buf[0] == 'Y';
}

static int install_package(const char *pkg_file) {
    char tmp_extract[MAX_PATH];
    SAFE_SNPRINTF(tmp_extract, "%s/extract_%d", g_tmpdir, (int)getpid());
    printf("OkraLinux: extracting %s\n", pkg_file);
    if (extract_okra(pkg_file, tmp_extract) != 0) {
        fprintf(stderr, "Error: failed to extract %s\n", pkg_file);
        return -1;
    }
    char meta_path[MAX_PATH];
    char base[MAX_PATH];
    PkgEntry pkg;
    int ret = -1;
    if (find_meta_in_dir(tmp_extract, meta_path, base) != 0) {
        fprintf(stderr, "Error: meta.yaml not found in package\n");
        goto cleanup;
    }
    if (parse_meta(meta_path, &pkg) != 0) {
        fprintf(stderr, "Error: failed to parse meta.yaml\n");
        goto cleanup;
    }
    printf("OkraLinux: package %s version %s\n", pkg.name, pkg.version);
    if (pkg.sha256[0] && verify_sha256(pkg_file, pkg.sha256) != 0) {
        fprintf(stderr, "Error: SHA256 checksum mismatch!\n");
        goto cleanup;
    }
    if (verify_signature(pkg_file) != 0) {
        fprintf(stderr, "Warning: signature verification failed\n");
        if (!g_ctx.force) goto cleanup;
    }
    if (check_conflicts(&pkg) != 0 && !g_ctx.force) goto cleanup;
    if (!g_ctx.nodeps) {
        for (int i = 0; i < pkg.dep_count; i++) {
            if (!db_is_installed(pkg.deps[i]) && !db_provides_installed(pkg.deps[i]))
                printf("  Warning: dependency %s not installed\n", pkg.deps[i]);
        }
    }
    char install_dir[MAX_PATH];
    SAFE_SNPRINTF(install_dir, "%s/%s-%s", STORE_DIR, pkg.name, pkg.version);
    char src_files[MAX_PATH];
    char pre_script[MAX_PATH];
    char post_script[MAX_PATH];
    if (base[0]) {
        SAFE_SNPRINTF(src_files, "%s/%s/files", tmp_extract, base);
        SAFE_SNPRINTF(pre_script, "%s/%s/scripts/pre-install", tmp_extract, base);
        SAFE_SNPRINTF(post_script, "%s/%s/scripts/post-install", tmp_extract, base);
    } else {
        SAFE_SNPRINTF(src_files, "%s/files", tmp_extract);
        SAFE_SNPRINTF(pre_script, "%s/scripts/pre-install", tmp_extract);
        SAFE_SNPRINTF(post_script, "%s/scripts/post-install", tmp_extract);
    }
    hook_run("install", "pre", pkg.name);
    run_script(pre_script);
    mkdir_p(install_dir);
    {
        char meta_copy[MAX_PATH];
        SAFE_SNPRINTF(meta_copy, "%s/meta.yaml", install_dir);
        safe_copy_file(meta_path, meta_copy);
    }
    if (dir_exists(src_files)) {
        char esc_src[MAX_PATH * 2], esc_dst[MAX_PATH * 2];
        shell_escape(src_files, esc_src, sizeof(esc_src));
        shell_escape(install_dir, esc_dst, sizeof(esc_dst));
        char cmd[MAX_PATH * 4];
        SAFE_SNPRINTF(cmd, "cp -a %s/. %s/", esc_src, esc_dst);
        system(cmd);
    }
    check_file_conflicts(install_dir);
    create_symlinks(install_dir, "usr/bin", "/usr/bin");
    create_symlinks(install_dir, "usr/lib", "/usr/lib");
    create_symlinks(install_dir, "usr/share/applications", "/usr/share/applications");
    create_symlinks(install_dir, "usr/share/icons", "/usr/share/icons");
    create_symlinks(install_dir, "usr/include", "/usr/include");
    create_symlinks(install_dir, "usr/share/man", "/usr/share/man");
    SAFE_COPY(pkg.install_path, install_dir);
    db_add(&pkg);
    run_script(post_script);
    hook_run("install", "post", pkg.name);
    printf("OkraLinux: %s-%s installed successfully\n", pkg.name, pkg.version);
    log_write("install", pkg.name, pkg.version, 1);
    ret = 0;
cleanup:
    safe_remove_tree(tmp_extract);
    return ret;
}

static int remove_package(const char *name) {
    PkgEntry pkg;
    if (db_get_entry(name, &pkg) != 0) {
        fprintf(stderr, "Error: package %s not found\n", name);
        return -1;
    }
    if (is_locked(name)) {
        fprintf(stderr, "Error: package %s is locked\n", name);
        return -1;
    }
    printf("OkraLinux: removing %s-%s\n", pkg.name, pkg.version);
    char pre_remove[MAX_PATH], post_remove[MAX_PATH];
    SAFE_SNPRINTF(pre_remove, "%s/scripts/pre-remove", pkg.install_path);
    SAFE_SNPRINTF(post_remove, "%s/scripts/post-remove", pkg.install_path);
    hook_run("remove", "pre", name);
    run_script(pre_remove);
    remove_symlinks(pkg.install_path, "usr/bin", "/usr/bin");
    remove_symlinks(pkg.install_path, "usr/lib", "/usr/lib");
    remove_symlinks(pkg.install_path, "usr/share/applications", "/usr/share/applications");
    remove_symlinks(pkg.install_path, "usr/share/icons", "/usr/share/icons");
    remove_symlinks(pkg.install_path, "usr/include", "/usr/include");
    remove_symlinks(pkg.install_path, "usr/share/man", "/usr/share/man");
    safe_remove_tree(pkg.install_path);
    db_remove(name);
    run_script(post_remove);
    hook_run("remove", "post", name);
    printf("OkraLinux: %s removed successfully\n", name);
    log_write("remove", name, pkg.version, 1);
    return 0;
}

static int reinstall_package(const char *name) {
    PkgEntry pkg;
    if (db_get_entry(name, &pkg) != 0) {
        fprintf(stderr, "Error: package %s not installed\n", name);
        return -1;
    }
    char pkg_file[MAX_PATH];
    if (download_package(name, pkg_file) != 0) {
        fprintf(stderr, "Error: cannot find package %s\n", name);
        return -1;
    }
    create_snapshot();
    remove_package(name);
    if (install_package(pkg_file) != 0) {
        fprintf(stderr, "Error: reinstall failed, rolling back\n");
        rollback_snapshot();
        return -1;
    }
    printf("OkraLinux: %s reinstalled\n", name);
    return 0;
}

static int find_orphans(char orphans[][MAX_NAME], int *count) {
    *count = 0;
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return -1;
    char installed[MAX_PKGS][MAX_NAME];
    int inst_count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f) && inst_count < MAX_PKGS) {
        if (sscanf(line, "%127s", installed[inst_count]) == 1) inst_count++;
    }
    fclose(f);
    for (int i = 0; i < inst_count; i++) {
        int is_dep = 0;
        for (int j = 0; j < inst_count && !is_dep; j++) {
            if (i == j) continue;
            PkgEntry pkg;
            if (db_get_entry(installed[j], &pkg) != 0) continue;
            for (int k = 0; k < pkg.dep_count; k++) {
                if (strcmp(pkg.deps[k], installed[i]) == 0 ||
                    (strcmp(pkg.name, installed[i]) == 0)) {
                    is_dep = 1;
                    break;
                }
            }
        }
        if (!is_dep && *count < MAX_PKGS) {
            SAFE_COPY(orphans[*count], installed[i]);
            (*count)++;
        }
    }
    return 0;
}

static int check_reverse_deps(const char *name) {
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return 0;
    int count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char db_name[MAX_NAME];
        if (sscanf(line, "%127s", db_name) != 1 || strcmp(db_name, name) == 0) continue;
        PkgEntry pkg;
        if (db_get_entry(db_name, &pkg) != 0) continue;
        for (int i = 0; i < pkg.dep_count; i++) {
            if (strcmp(pkg.deps[i], name) == 0) {
                printf("  %s depends on %s\n", db_name, name);
                count++;
            }
        }
    }
    fclose(f);
    return count;
}

static int verify_package_files(const char *name) {
    PkgEntry pkg;
    if (db_get_entry(name, &pkg) != 0) {
        fprintf(stderr, "Error: %s not installed\n", name);
        return -1;
    }
    printf("Verifying %s-%s...\n", pkg.name, pkg.version);
    char esc[MAX_PATH * 2];
    shell_escape(pkg.install_path, esc, sizeof(esc));
    char cmd[MAX_PATH * 3];
    SAFE_SNPRINTF(cmd, "find %s -type f 2>/dev/null | wc -l", esc);
    FILE *p = popen(cmd, "r");
    int total = 0;
    if (p) {
        char buf[16];
        if (fgets(buf, sizeof(buf), p)) total = atoi(buf);
        pclose(p);
    }
    printf("  %d files in install path\n", total);
    char bin_dir[MAX_PATH];
    SAFE_SNPRINTF(bin_dir, "%s/usr/bin", pkg.install_path);
    if (dir_exists(bin_dir)) {
        DIR *bd = opendir(bin_dir);
        if (bd) {
            struct dirent *entry;
            int broken = 0;
            while ((entry = readdir(bd))) {
                if (entry->d_name[0] == '.') continue;
                char link_path[MAX_PATH];
                SAFE_SNPRINTF(link_path, "/usr/bin/%s", entry->d_name);
                struct stat st;
                if (lstat(link_path, &st) != 0) {
                    printf("  MISSING: /usr/bin/%s\n", entry->d_name);
                    broken++;
                } else if (S_ISLNK(st.st_mode)) {
                    char target[MAX_PATH];
                    int n = readlink(link_path, target, sizeof(target) - 1);
                    if (n > 0) {
                        target[n] = '\0';
                        if (!file_exists(target)) {
                            printf("  BROKEN: /usr/bin/%s -> %s\n", entry->d_name, target);
                            broken++;
                        }
                    }
                }
            }
            closedir(bd);
            if (broken == 0) printf("  all symlinks OK\n");
            else printf("  %d broken/missing symlinks\n", broken);
        }
    }
    return 0;
}

static int export_installed(const char *out_file) {
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return -1;
    FILE *out = fopen(out_file, "w");
    if (!out) { fclose(f); return -1; }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char name[MAX_NAME], version[MAX_VERSION];
        if (sscanf(line, "%127s %63s", name, version) == 2)
            fprintf(out, "%s %s\n", name, version);
    }
    fclose(f);
    fclose(out);
    printf("OkraLinux: exported installed packages to %s\n", out_file);
    return 0;
}

static int import_installed(const char *in_file) {
    FILE *f = fopen(in_file, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    int count = 0, installed = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[MAX_NAME], version[MAX_VERSION];
        if (sscanf(line, "%127s %63s", name, version) == 2) count++;
    }
    rewind(f);
    printf("OkraLinux: importing %d packages from %s\n", count, in_file);
    create_snapshot();
    while (fgets(line, sizeof(line), f)) {
        char name[MAX_NAME], version[MAX_VERSION];
        if (sscanf(line, "%127s %63s", name, version) != 2) continue;
        if (db_is_installed(name)) {
            printf("  %s already installed, skipping\n", name);
            continue;
        }
        printf("  installing %s\n", name);
        char pkg_file[MAX_PATH];
        if (download_package(name, pkg_file) == 0) {
            if (install_package(pkg_file) == 0) installed++;
        }
    }
    fclose(f);
    printf("OkraLinux: imported %d/%d packages\n", installed, count);
    return 0;
}

static int render_dep_tree(const char *name, int depth, int max_depth,
                           const char *visited[], int *visited_count) {
    if (depth > max_depth) return 0;
    if (has_visited(visited, *visited_count, name)) {
        for (int i = 0; i < depth; i++) printf("  ");
        printf("%s [cycle detected]\n", name);
        return 0;
    }
    if (*visited_count >= MAX_VISITED) return 0;
    SAFE_COPY((char *)visited[*visited_count], name);
    (*visited_count)++;
    for (int i = 0; i < depth; i++) printf("  ");
    PkgEntry pkg;
    if (db_get_entry(name, &pkg) != 0) {
        char pkg_file[MAX_PATH];
        if (download_package(name, pkg_file) != 0) {
            printf("%s [not found]\n", name);
            return -1;
        }
        char tmp_extract[MAX_PATH];
        SAFE_SNPRINTF(tmp_extract, "%s/tree_%d_%d", g_tmpdir, depth, (int)getpid());
        extract_okra(pkg_file, tmp_extract);
        char meta_path[MAX_PATH], base[MAX_PATH];
        if (find_meta_in_dir(tmp_extract, meta_path, base) != 0) {
            printf("%s [no meta]\n", name);
            safe_remove_tree(tmp_extract);
            return -1;
        }
        parse_meta(meta_path, &pkg);
        safe_remove_tree(tmp_extract);
    }
    if (pkg.installed) printf("%s-%s [installed]\n", pkg.name, pkg.version);
    else printf("%s-%s\n", pkg.name, pkg.version);
    for (int i = 0; i < pkg.dep_count; i++) {
        for (int j = 0; j < depth + 1; j++) printf("  ");
        printf("-> %s\n", pkg.deps[i]);
        render_dep_tree(pkg.deps[i], depth + 1, max_depth, visited, visited_count);
    }
    return 0;
}

static int render_rdep_tree(const char *name, int depth, int max_depth,
                            const char *visited[], int *visited_count) {
    if (depth > max_depth) return 0;
    if (has_visited(visited, *visited_count, name)) return 0;
    SAFE_COPY((char *)visited[*visited_count], name);
    (*visited_count)++;
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char db_name[MAX_NAME];
        if (sscanf(line, "%127s", db_name) != 1 || strcmp(db_name, name) == 0) continue;
        PkgEntry pkg;
        if (db_get_entry(db_name, &pkg) != 0) continue;
        for (int i = 0; i < pkg.dep_count; i++) {
            if (strcmp(pkg.deps[i], name) == 0) {
                for (int j = 0; j < depth; j++) printf("  ");
                printf("%s depends on %s\n", db_name, name);
                render_rdep_tree(db_name, depth + 1, max_depth, visited, visited_count);
            }
        }
    }
    fclose(f);
    return 0;
}

static int find_provider(const char *query) {
    int found = 0;
    FILE *f = fopen(DB_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            char name[MAX_NAME], ver[MAX_VERSION], path[MAX_PATH];
            if (sscanf(line, "%127s %63s %511s", name, ver, path) != 3) continue;
            char bin_dir[MAX_PATH];
            SAFE_SNPRINTF(bin_dir, "%s/usr/bin", path);
            if (!dir_exists(bin_dir)) continue;
            DIR *bd = opendir(bin_dir);
            if (!bd) continue;
            struct dirent *entry;
            while ((entry = readdir(bd))) {
                if (entry->d_name[0] == '.') continue;
                if (strcmp(entry->d_name, query) == 0) {
                    printf("  /usr/bin/%s is provided by %s-%s\n", query, name, ver);
                    found++;
                }
            }
            closedir(bd);
        }
        fclose(f);
    }
    return found;
}

static void print_size_human(long bytes) {
    if (bytes < 1024) printf("%ld B", bytes);
    else if (bytes < 1024 * 1024) printf("%.1f KB", bytes / 1024.0);
    else if (bytes < 1024 * 1024 * 1024) printf("%.1f MB", bytes / (1024.0 * 1024));
    else printf("%.1f GB", bytes / (1024.0 * 1024 * 1024));
}

static int group_install(const char *group) {
    char path[MAX_PATH];
    SAFE_SNPRINTF(path, "%s/%s.members", DB_DIR, group);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    int count = 0;
    char pkgs[MAX_PKGS][MAX_NAME];
    while (fgets(line, sizeof(line), f) && count < MAX_PKGS) {
        char *m = trim(line);
        if (*m) { SAFE_COPY(pkgs[count], m); count++; }
    }
    fclose(f);
    printf("OkraLinux: installing group '%s' (%d packages)\n", group, count);
    create_snapshot();
    for (int i = 0; i < count; i++) {
        char pkg_file[MAX_PATH];
        if (!db_is_installed(pkgs[i])) {
            printf("  [%d/%d] %s\n", i + 1, count, pkgs[i]);
            if (download_package(pkgs[i], pkg_file) == 0)
                install_package(pkg_file);
        }
    }
    return 0;
}

static int cmd_install(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm install <pkg> [pkg...]\n"); return 1; }
    create_snapshot();
    int failures = 0;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-') continue;
        char pkg_file[MAX_PATH];
        if (strstr(argv[i], ".okra") || file_exists(argv[i])) {
            SAFE_COPY(pkg_file, argv[i]);
        } else {
            char *at = strchr(argv[i], '@');
            char name[MAX_NAME];
            if (at) {
                size_t len = at - argv[i];
                if (len >= MAX_NAME) len = MAX_NAME - 1;
                memcpy(name, argv[i], len);
                name[len] = '\0';
            } else {
                SAFE_COPY(name, argv[i]);
            }
            if (db_is_installed(name)) {
                printf("OkraLinux: %s already installed, skipping\n", name);
                continue;
            }
            if (!g_ctx.nodeps) {
                int resolved = 0;
                char visited[MAX_VISITED][MAX_NAME];
                memset(visited, 0, sizeof(visited));
                int visited_count = 0;
                if (resolve_deps(name, 0, &resolved,
                                 (const char **)visited, &visited_count) != 0) {
                    if (!g_ctx.force) {
                        fprintf(stderr, "Error: dependency resolution failed for %s\n", name);
                        failures++;
                        continue;
                    }
                }
            }
            if (db_is_installed(name)) continue;
            if (download_package(name, pkg_file) != 0) {
                if (!g_ctx.force) {
                    fprintf(stderr, "Error: cannot find package %s\n", name);
                    failures++;
                    continue;
                }
            }
        }
        if (g_ctx.download_only) {
            printf("OkraLinux: downloaded (install skipped): %s\n", pkg_file);
            continue;
        }
        if (install_package(pkg_file) != 0) {
            fprintf(stderr, "Error: failed to install %s\n", argv[i]);
            failures++;
            if (!g_ctx.force) {
                fprintf(stderr, "OkraLinux: rolling back due to failure\n");
                rollback_snapshot();
            }
        }
    }
    cleanup_old_snapshots(10);
    if (failures > 0) {
        fprintf(stderr, "OkraLinux: %d package(s) failed\n", failures);
        return 1;
    }
    return 0;
}

static int cmd_remove(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm remove <pkg> [pkg...]\n"); return 1; }
    if (!confirm_action("Remove selected packages?")) return 0;
    create_snapshot();
    int failures = 0;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        int rdeps = check_reverse_deps(argv[i]);
        if (rdeps > 0 && !g_ctx.force) {
            fprintf(stderr, "Error: %s is needed by %d other package(s) (use --force)\n", argv[i], rdeps);
            failures++;
            continue;
        }
        if (remove_package(argv[i]) != 0) failures++;
    }
    cleanup_old_snapshots(10);
    if (failures > 0) { fprintf(stderr, "OkraLinux: %d package(s) failed\n", failures); return 1; }
    return 0;
}

static int cmd_purge(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm purge <pkg> [pkg...]\n"); return 1; }
    if (!confirm_action("Purge selected packages (all versions)?")) return 0;
    create_snapshot();
    for (int i = 2; i < argc; i++) {
        PkgEntry pkg;
        if (db_get_entry(argv[i], &pkg) != 0) {
            fprintf(stderr, "Error: %s not found\n", argv[i]);
            continue;
        }
        remove_symlinks(pkg.install_path, "usr/bin", "/usr/bin");
        remove_symlinks(pkg.install_path, "usr/lib", "/usr/lib");
        remove_symlinks(pkg.install_path, "usr/share/applications", "/usr/share/applications");
        remove_symlinks(pkg.install_path, "usr/share/icons", "/usr/share/icons");
        char esc[MAX_PATH * 2];
        char glob_path[MAX_PATH];
        SAFE_SNPRINTF(glob_path, "%s/%s-*", STORE_DIR, pkg.name);
        shell_escape(glob_path, esc, sizeof(esc));
        char cmd[MAX_PATH * 3];
        SAFE_SNPRINTF(cmd, "rm -rf %s 2>/dev/null", esc);
        system(cmd);
        db_remove(argv[i]);
        lock_remove(argv[i]);
        printf("OkraLinux: purged %s (all versions)\n", argv[i]);
        log_write("purge", argv[i], "", 1);
    }
    return 0;
}

static int cmd_update(void) {
    printf("OkraLinux: checking for updates...\n");
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    int upgradable = 0, checked = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[MAX_NAME], version[MAX_VERSION];
        if (sscanf(line, "%127s %63s", name, version) != 2) continue;
        checked++;
        if (is_locked(name)) { printf("  %s-%s [locked]\n", name, version); continue; }
        FILE *rf = fopen(REPO_FILE, "r");
        if (!rf) break;
        char rline[MAX_LINE];
        int found = 0;
        while (fgets(rline, sizeof(rline), rf) && !found) {
            char url[MAX_URL], type[16], arch[32];
            int pri, enabled;
            if (sscanf(rline, "%1023s %d %15s %31s %d", url, &pri, type, arch, &enabled) < 4) continue;
            if (!enabled) continue;
            char meta_url[MAX_URL * 2];
            SAFE_SNPRINTF(meta_url, "%s/%s/meta.yaml", url, name);
            char tmp_meta[MAX_PATH];
            SAFE_SNPRINTF(tmp_meta, "%s/%s_meta.yaml", g_tmpdir, name);
            char esc_url[MAX_URL * 2], esc_meta[MAX_PATH * 2];
            shell_escape(meta_url, esc_url, sizeof(esc_url));
            shell_escape(tmp_meta, esc_meta, sizeof(esc_meta));
            char cmd[MAX_URL * 3 + MAX_PATH * 3];
            SAFE_SNPRINTF(cmd, "wget -q --tries=1 --timeout=10 %s -O %s 2>/dev/null", esc_url, esc_meta);
            if (system(cmd) == 0 && file_exists(tmp_meta)) {
                PkgEntry remote;
                if (parse_meta(tmp_meta, &remote) == 0) {
                    if (strcmp(remote.version, version) != 0) {
                        printf("  %s: %s -> %s [upgradable]\n", name, version, remote.version);
                        upgradable++;
                    } else printf("  %s-%s [latest]\n", name, version);
                    found = 1;
                }
                unlink(tmp_meta);
            }
        }
        fclose(rf);
        if (!found) printf("  %s-%s [no remote info]\n", name, version);
    }
    fclose(f);
    if (checked == 0) printf("No packages installed\n");
    else if (upgradable == 0) printf("All packages are up to date\n");
    else printf("\n%d package(s) can be upgraded\n", upgradable);
    return 0;
}

static int cmd_upgrade(int argc, char **argv) {
    if (argc >= 3 && argv[2][0] != '-') {
        char *name = argv[2];
        PkgEntry pkg;
        if (db_get_entry(name, &pkg) != 0) { fprintf(stderr, "Error: %s not installed\n", name); return -1; }
        if (is_locked(name)) { fprintf(stderr, "Error: %s is locked\n", name); return -1; }
        create_snapshot();
        char pkg_file[MAX_PATH];
        if (download_package(name, pkg_file) != 0) { fprintf(stderr, "Error: cannot find package %s\n", name); return -1; }
        if (remove_package(name) != 0) { fprintf(stderr, "Error: failed to remove old version\n"); rollback_snapshot(); return -1; }
        if (install_package(pkg_file) != 0) { fprintf(stderr, "Error: upgrade failed, rolling back\n"); rollback_snapshot(); return -1; }
        printf("OkraLinux: %s upgraded\n", name);
        cleanup_old_snapshots(10);
        return 0;
    }
    create_snapshot();
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return -1;
    char pkgs[MAX_PKGS][MAX_NAME];
    int count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f) && count < MAX_PKGS)
        if (sscanf(line, "%127s", pkgs[count]) == 1) count++;
    fclose(f);
    int failures = 0, upgraded = 0;
    for (int i = 0; i < count; i++) {
        if (is_locked(pkgs[i])) { printf("OkraLinux: skipping locked %s\n", pkgs[i]); continue; }
        char pkg_file[MAX_PATH];
        if (download_package(pkgs[i], pkg_file) == 0) {
            remove_package(pkgs[i]);
            if (install_package(pkg_file) != 0) {
                fprintf(stderr, "Error: failed to upgrade %s\n", pkgs[i]);
                failures++;
            } else upgraded++;
        }
    }
    cleanup_old_snapshots(10);
    if (failures > 0) {
        fprintf(stderr, "OkraLinux: %d failed, %d upgraded\n", failures, upgraded);
        if (upgraded == 0) rollback_snapshot();
        return 1;
    }
    printf("OkraLinux: %d package(s) upgraded\n", upgraded);
    return 0;
}

static int cmd_rollback(void) { return rollback_snapshot(); }

static int cmd_list(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "--installed") == 0) {
        FILE *f = fopen(DB_FILE, "r");
        if (!f) return -1;
        char line[MAX_LINE];
        int count = 0;
        printf("%-30s %-20s %-15s %s\n", "NAME", "VERSION", "SIZE", "PATH");
        printf("%-30s %-20s %-15s %s\n", "----", "-------", "----", "----");
        while (fgets(line, sizeof(line), f)) {
            char name[MAX_NAME], ver[MAX_VERSION], path[MAX_PATH];
            if (sscanf(line, "%127s %63s %511s", name, ver, path) == 3) {
                long sz = 0;
                char esc[MAX_PATH * 2];
                shell_escape(path, esc, sizeof(esc));
                char cmd[MAX_PATH * 3];
                SAFE_SNPRINTF(cmd, "du -sb %s 2>/dev/null | awk '{print $1}'", esc);
                FILE *p = popen(cmd, "r");
                if (p) { char buf[32]; if (fgets(buf, sizeof(buf), p)) sz = atol(buf); pclose(p); }
                char size_str[32];
                if (sz < 1024) SAFE_SNPRINTF(size_str, "%ld B", sz);
                else if (sz < 1048576) SAFE_SNPRINTF(size_str, "%.1f KB", sz / 1024.0);
                else SAFE_SNPRINTF(size_str, "%.1f MB", sz / (1024.0 * 1024));
                printf("%-30s %-20s %-15s %s\n", name, ver, size_str, path);
                count++;
            }
        }
        fclose(f);
        if (count == 0) printf("(no packages installed)\n");
        else printf("\nTotal: %d packages\n", count);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "--upgradable") == 0) return cmd_update();
    if (argc >= 3 && strcmp(argv[2], "--repos") == 0) {
        FILE *f = fopen(REPO_FILE, "r");
        if (!f) return -1;
        char line[MAX_LINE];
        int count = 0;
        printf("%-5s %-10s %-10s %-8s %s\n", "PRI", "TYPE", "ARCH", "ENABLED", "URL");
        printf("%-5s %-10s %-10s %-8s %s\n", "---", "----", "----", "-------", "---");
        while (fgets(line, sizeof(line), f)) {
            char url[MAX_URL], type[16], arch[32];
            int pri, enabled;
            if (sscanf(line, "%1023s %d %15s %31s %d", url, &pri, type, arch, &enabled) >= 4) {
                printf("%-5d %-10s %-10s %-8s %s\n", pri, type, arch, enabled ? "yes" : "no", url);
                count++;
            }
        }
        fclose(f);
        if (count == 0) printf("(no sources)\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "--locks") == 0) {
        FILE *f = fopen(LOCK_FILE, "r");
        if (!f) return -1;
        char line[MAX_LINE];
        int count = 0;
        printf("%-30s %s\n", "NAME", "VERSION");
        printf("%-30s %s\n", "----", "-------");
        while (fgets(line, sizeof(line), f)) {
            char name[MAX_NAME], version[MAX_VERSION];
            if (sscanf(line, "%127s %63s", name, version) == 2) { printf("%-30s %s\n", name, version); count++; }
        }
        fclose(f);
        if (count == 0) printf("(no locked packages)\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "--snapshots") == 0) {
        DIR *d = opendir(SNAP_DIR);
        if (!d) return -1;
        struct dirent *entry;
        int count = 0;
        printf("%-30s %s\n", "SNAPSHOT", "TIME");
        printf("%-30s %s\n", "--------", "----");
        while ((entry = readdir(d))) {
            if (entry->d_name[0] == '.') continue;
            char path[MAX_PATH];
            SAFE_SNPRINTF(path, "%s/%s", SNAP_DIR, entry->d_name);
            struct stat st;
            if (stat(path, &st) == 0) {
                struct tm *tm = localtime(&st.st_mtime);
                char tbuf[32];
                SAFE_SNPRINTF(tbuf, "%04d-%02d-%02d %02d:%02d",
                         tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
                printf("%-30s %s\n", entry->d_name, tbuf);
                count++;
            }
        }
        closedir(d);
        if (count == 0) printf("(no snapshots)\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "--orphans") == 0) {
        char orphans[MAX_PKGS][MAX_NAME];
        int count;
        find_orphans(orphans, &count);
        if (count == 0) printf("No orphan packages\n");
        else for (int i = 0; i < count; i++) printf("  %s\n", orphans[i]);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[2], "--groups") == 0) {
        FILE *f = fopen(GROUP_FILE, "r");
        if (!f) return -1;
        char line[MAX_LINE];
        int count = 0;
        printf("%-30s %s\n", "GROUP", "DESCRIPTION");
        printf("%-30s %s\n", "-----", "-----------");
        while (fgets(line, sizeof(line), f)) {
            char name[MAX_NAME], desc[MAX_DESC];
            if (sscanf(line, "%127s %255[^\n]", name, desc) >= 1) {
                printf("%-30s %s\n", name, desc[0] ? desc : "");
                count++;
            }
        }
        fclose(f);
        if (count == 0) printf("(no groups)\n");
        return 0;
    }
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[MAX_NAME], ver[MAX_VERSION];
        if (sscanf(line, "%127s %63s", name, ver) == 2) { printf("%s-%s\n", name, ver); count++; }
    }
    fclose(f);
    if (count == 0) printf("(no packages installed)\n");
    return 0;
}

static int cmd_search(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm search <keyword>\n"); return 1; }
    char *keyword = argv[2];
    int found = 0;
    FILE *f = fopen(DB_FILE, "r");
    if (f) {
        printf("Installed:\n");
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            char name[MAX_NAME], version[MAX_VERSION], path[MAX_PATH];
            if (sscanf(line, "%127s %63s %511s", name, version, path) >= 2) {
                PkgEntry pkg;
                int match = (strstr(name, keyword) != NULL);
                if (db_get_entry(name, &pkg) == 0 && pkg.desc[0] && strstr(pkg.desc, keyword))
                    match = 1;
                if (match) { printf("  %s-%s %s\n", name, version, pkg.desc[0] ? pkg.desc : ""); found++; }
            }
        }
        fclose(f);
    }
    FILE *rf = fopen(REPO_FILE, "r");
    if (rf) {
        printf("Remote:\n");
        char rline[MAX_LINE];
        while (fgets(rline, sizeof(rline), rf)) {
            char url[MAX_URL], type[16], arch[32];
            int pri, enabled;
            if (sscanf(rline, "%1023s %d %15s %31s %d", url, &pri, type, arch, &enabled) < 4 || !enabled) continue;
            char list_url[MAX_URL * 2];
            SAFE_SNPRINTF(list_url, "%s/list.txt", url);
            char tmp_list[MAX_PATH];
            SAFE_SNPRINTF(tmp_list, "%s/repo_list_%d.txt", g_tmpdir, (int)getpid());
            char esc_url[MAX_URL * 2], esc_tmp[MAX_PATH * 2];
            shell_escape(list_url, esc_url, sizeof(esc_url));
            shell_escape(tmp_list, esc_tmp, sizeof(esc_tmp));
            char cmd[MAX_URL * 3 + MAX_PATH * 3];
            SAFE_SNPRINTF(cmd, "wget -q --tries=1 --timeout=10 %s -O %s 2>/dev/null", esc_url, esc_tmp);
            if (system(cmd) == 0 && file_exists(tmp_list)) {
                FILE *lf = fopen(tmp_list, "r");
                if (lf) {
                    char lline[MAX_LINE];
                    while (fgets(lline, sizeof(lline), lf)) {
                        char *trimmed = trim(lline);
                        if (strstr(trimmed, keyword)) { printf("  [remote] %s\n", trimmed); found++; }
                    }
                    fclose(lf);
                    unlink(tmp_list);
                }
            }
        }
        fclose(rf);
    }
    if (!found) printf("No packages found matching '%s'\n", keyword);
    return 0;
}

static int cmd_info(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm info <pkg>\n"); return 1; }
    char *name = argv[2];
    PkgEntry pkg;
    if (db_get_entry(name, &pkg) == 0) {
        printf("Name: %s\n", pkg.name);
        printf("Version: %s\n", pkg.version);
        printf("Description: %s\n", pkg.desc[0] ? pkg.desc : "(none)");
        printf("Architecture: %s\n", pkg.arch[0] ? pkg.arch : "any");
        printf("Install Path: %s\n", pkg.install_path);
        printf("Source: %s\n", pkg.source[0] ? pkg.source : "(none)");
        printf("Status: installed\n");
        if (is_locked(name)) printf("Locked: yes\n");
        char esc[MAX_PATH * 2];
        shell_escape(pkg.install_path, esc, sizeof(esc));
        char cmd[MAX_PATH * 3];
        SAFE_SNPRINTF(cmd, "du -sh %s 2>/dev/null | awk '{print $1}'", esc);
        FILE *p = popen(cmd, "r");
        if (p) { char buf[32]; if (fgets(buf, sizeof(buf), p)) printf("Installed Size: %s", buf); pclose(p); }
        if (pkg.dep_count > 0) {
            printf("Dependencies:\n");
            for (int i = 0; i < pkg.dep_count; i++) {
                int installed = db_is_installed(pkg.deps[i]) || db_provides_installed(pkg.deps[i]);
                printf("  - %s %s\n", pkg.deps[i], installed ? "[installed]" : "[missing]");
            }
        }
        if (pkg.conflict_count > 0) { printf("Conflicts:\n"); for (int i = 0; i < pkg.conflict_count; i++) printf("  - %s\n", pkg.conflicts[i]); }
        if (pkg.provide_count > 0) { printf("Provides:\n"); for (int i = 0; i < pkg.provide_count; i++) printf("  - %s\n", pkg.provides[i]); }
        if (pkg.replace_count > 0) { printf("Replaces:\n"); for (int i = 0; i < pkg.replace_count; i++) printf("  - %s\n", pkg.replaces[i]); }
        if (pkg.group_count > 0) { printf("Groups:\n"); for (int i = 0; i < pkg.group_count; i++) printf("  - %s\n", pkg.groups[i]); }
        char bin_dir[MAX_PATH];
        SAFE_SNPRINTF(bin_dir, "%s/usr/bin", pkg.install_path);
        if (dir_exists(bin_dir)) {
            printf("Binaries:\n");
            DIR *bd = opendir(bin_dir);
            if (bd) {
                struct dirent *entry;
                while ((entry = readdir(bd))) {
                    if (entry->d_name[0] != '.') printf("  /usr/bin/%s\n", entry->d_name);
                }
                closedir(bd);
            }
        }
        return 0;
    }
    FILE *rf = fopen(REPO_FILE, "r");
    if (!rf) { fprintf(stderr, "Error: %s not found\n", name); return -1; }
    char line[MAX_LINE];
    int found = 0;
    while (fgets(line, sizeof(line), rf) && !found) {
        char url[MAX_URL], type[16], arch[32];
        int pri, enabled;
        if (sscanf(line, "%1023s %d %15s %31s %d", url, &pri, type, arch, &enabled) < 4 || !enabled) continue;
        char meta_url[MAX_URL * 2];
        SAFE_SNPRINTF(meta_url, "%s/%s/meta.yaml", url, name);
        char tmp_meta[MAX_PATH];
        SAFE_SNPRINTF(tmp_meta, "%s/%s_meta.yaml", g_tmpdir, name);
        char esc_url[MAX_URL * 2], esc_meta[MAX_PATH * 2];
        shell_escape(meta_url, esc_url, sizeof(esc_url));
        shell_escape(tmp_meta, esc_meta, sizeof(esc_meta));
        char cmd[MAX_URL * 3 + MAX_PATH * 3];
        SAFE_SNPRINTF(cmd, "wget -q --tries=1 --timeout=10 %s -O %s 2>/dev/null", esc_url, esc_meta);
        if (system(cmd) == 0 && file_exists(tmp_meta)) {
            if (parse_meta(tmp_meta, &pkg) == 0) {
                printf("Name: %s\n", pkg.name);
                printf("Version: %s\n", pkg.version);
                printf("Description: %s\n", pkg.desc[0] ? pkg.desc : "(none)");
                printf("Architecture: %s\n", pkg.arch[0] ? pkg.arch : "any");
                printf("Source: %s\n", pkg.source[0] ? pkg.source : url);
                printf("Status: available (not installed)\n");
                if (pkg.sha256[0]) printf("SHA256: %s\n", pkg.sha256);
                if (pkg.size > 0) { printf("Size: "); print_size_human(pkg.size); printf("\n"); }
                if (pkg.dep_count > 0) { printf("Dependencies:\n"); for (int i = 0; i < pkg.dep_count; i++) printf("  - %s\n", pkg.deps[i]); }
                if (pkg.conflict_count > 0) { printf("Conflicts:\n"); for (int i = 0; i < pkg.conflict_count; i++) printf("  - %s\n", pkg.conflicts[i]); }
                if (pkg.provide_count > 0) { printf("Provides:\n"); for (int i = 0; i < pkg.provide_count; i++) printf("  - %s\n", pkg.provides[i]); }
                if (pkg.replace_count > 0) { printf("Replaces:\n"); for (int i = 0; i < pkg.replace_count; i++) printf("  - %s\n", pkg.replaces[i]); }
                if (pkg.group_count > 0) { printf("Groups:\n"); for (int i = 0; i < pkg.group_count; i++) printf("  - %s\n", pkg.groups[i]); }
                found = 1;
            }
            unlink(tmp_meta);
        }
    }
    fclose(rf);
    if (!found) { fprintf(stderr, "Error: %s not found\n", name); return -1; }
    return 0;
}

static int cmd_files(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm files <pkg>\n"); return 1; }
    PkgEntry pkg;
    if (db_get_entry(argv[2], &pkg) != 0) { fprintf(stderr, "Error: %s not installed\n", argv[2]); return -1; }
    printf("Files owned by %s-%s:\n", pkg.name, pkg.version);
    char esc[MAX_PATH * 2];
    shell_escape(pkg.install_path, esc, sizeof(esc));
    char cmd[MAX_PATH * 3];
    SAFE_SNPRINTF(cmd, "find %s -type f -o -type l 2>/dev/null | sort", esc);
    return system(cmd);
}

static int cmd_add_source(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm add-source <url> [priority] [type] [arch]\n"); return 1; }
    char *url = argv[2];
    int pri = (argc >= 4) ? atoi(argv[3]) : 1;
    char *type = (argc >= 5) ? argv[4] : "http";
    char *arch = (argc >= 6) ? argv[5] : "any";
    if (repo_add(url, pri, type, arch) != 0) { fprintf(stderr, "Error: failed to add source\n"); return -1; }
    printf("OkraLinux: source added: %s (priority=%d, type=%s, arch=%s)\n", url, pri, type, arch);
    log_write("add-source", url, "", 1);
    return 0;
}

static int cmd_remove_source(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm remove-source <url>\n"); return 1; }
    if (repo_remove(argv[2]) != 0) { printf("OkraLinux: source not found: %s\n", argv[2]); return 0; }
    printf("OkraLinux: source removed: %s\n", argv[2]);
    log_write("remove-source", argv[2], "", 1);
    return 0;
}

static int cmd_build(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm build <source_dir>\n"); return 1; }
    char *src_dir = argv[2];
    char meta_path[MAX_PATH];
    SAFE_SNPRINTF(meta_path, "%s/meta.yaml", src_dir);
    if (!file_exists(meta_path)) { fprintf(stderr, "Error: meta.yaml not found in %s\n", src_dir); return -1; }
    PkgEntry pkg;
    if (parse_meta(meta_path, &pkg) != 0) { fprintf(stderr, "Error: failed to parse meta.yaml\n"); return -1; }
    char out_name[MAX_PATH];
    SAFE_SNPRINTF(out_name, "%s-%s.okra", pkg.name, pkg.version);
    printf("OkraLinux: building %s from %s\n", out_name, src_dir);
    char pre_build[MAX_PATH];
    SAFE_SNPRINTF(pre_build, "%s/scripts/pre-build", src_dir);
    run_script(pre_build);
    char esc_src[MAX_PATH * 2], esc_out[MAX_PATH * 2];
    shell_escape(src_dir, esc_src, sizeof(esc_src));
    shell_escape(out_name, esc_out, sizeof(esc_out));
    char cmd[MAX_PATH * 6];
    if (has_tool("zstd"))
        SAFE_SNPRINTF(cmd, "tar --zstd -cf %s -C %s . 2>/dev/null", esc_out, esc_src);
    else if (has_tool("xz"))
        SAFE_SNPRINTF(cmd, "tar -cJf %s -C %s . 2>/dev/null", esc_out, esc_src);
    else
        SAFE_SNPRINTF(cmd, "tar -cf %s -C %s .", esc_out, esc_src);
    int ret = system(cmd);
    if (ret == 0) {
        printf("OkraLinux: built %s (%ld bytes)\n", out_name, file_size(out_name));
        if (pkg.sha256[0]) {
            char actual[MAX_HASH];
            compute_sha256(out_name, actual);
            printf("  SHA256: %s\n", actual);
        }
        char post_build[MAX_PATH];
        SAFE_SNPRINTF(post_build, "%s/scripts/post-build", src_dir);
        run_script(post_build);
        log_write("build", pkg.name, pkg.version, 1);
    } else fprintf(stderr, "Error: build failed\n");
    return ret;
}

static int cmd_lock(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm lock <pkg> [version]\n"); return 1; }
    char *name = argv[2];
    char *version = (argc >= 4) ? argv[3] : "current";
    PkgEntry pkg;
    if (db_get_entry(name, &pkg) != 0) { fprintf(stderr, "Error: %s not installed\n", name); return -1; }
    if (lock_add(name, version) != 0) { fprintf(stderr, "Error: failed to lock %s\n", name); return -1; }
    printf("OkraLinux: locked %s at %s\n", name, version);
    return 0;
}

static int cmd_unlock(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm unlock <pkg>\n"); return 1; }
    if (lock_remove(argv[2]) != 0) { fprintf(stderr, "Error: failed to unlock %s\n", argv[2]); return -1; }
    printf("OkraLinux: unlocked %s\n", argv[2]);
    return 0;
}

static int cmd_clean(void) {
    safe_remove_tree(g_tmpdir);
    mkdir_p(g_tmpdir);
    long cache_size = 0;
    DIR *d = opendir(CACHE_DIR);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d))) {
            if (entry->d_name[0] == '.') continue;
            char path[MAX_PATH];
            SAFE_SNPRINTF(path, "%s/%s", CACHE_DIR, entry->d_name);
            cache_size += file_size(path);
        }
        closedir(d);
    }
    printf("OkraLinux: cleaned temp files\n");
    printf("  cache: %s (%ld bytes)\n", CACHE_DIR, cache_size);
    if (cache_size > 0) {
        if (confirm_action("Clean package cache?")) {
            char esc[MAX_PATH * 2];
            char glob[MAX_PATH];
            SAFE_SNPRINTF(glob, "%s/*", CACHE_DIR);
            shell_escape(glob, esc, sizeof(esc));
            char cmd[MAX_PATH * 3];
            SAFE_SNPRINTF(cmd, "rm -rf %s 2>/dev/null", esc);
            system(cmd);
            mkdir_p(CACHE_DIR);
            printf("  cache cleared\n");
        }
    }
    int old = cleanup_old_snapshots(10);
    if (old > 0) printf("  cleaned %d old snapshot(s)\n", old);
    return 0;
}

static int cmd_autoremove(void) {
    char orphans[MAX_PKGS][MAX_NAME];
    int count;
    find_orphans(orphans, &count);
    if (count == 0) { printf("No orphan packages to remove\n"); return 0; }
    printf("Orphan packages (%d):\n", count);
    for (int i = 0; i < count; i++) printf("  %s\n", orphans[i]);
    if (!confirm_action("Remove orphan packages?")) return 0;
    create_snapshot();
    for (int i = 0; i < count; i++) remove_package(orphans[i]);
    cleanup_old_snapshots(10);
    printf("OkraLinux: removed %d orphan packages\n", count);
    return 0;
}

static int cmd_depends(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm depends <pkg> [depth]\n"); return 1; }
    int max_depth = (argc >= 4) ? atoi(argv[3]) : 5;
    char visited[MAX_VISITED][MAX_NAME];
    memset(visited, 0, sizeof(visited));
    int visited_count = 0;
    render_dep_tree(argv[2], 0, max_depth, (const char **)visited, &visited_count);
    return 0;
}

static int cmd_rdepends(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm rdepends <pkg> [depth]\n"); return 1; }
    int max_depth = (argc >= 4) ? atoi(argv[3]) : 3;
    printf("Reverse dependencies of %s:\n", argv[2]);
    char visited[MAX_VISITED][MAX_NAME];
    memset(visited, 0, sizeof(visited));
    int visited_count = 0;
    render_rdep_tree(argv[2], 0, max_depth, (const char **)visited, &visited_count);
    return 0;
}

static int cmd_check(int argc, char **argv) {
    if (argc < 3) {
        FILE *f = fopen(DB_FILE, "r");
        if (!f) return -1;
        char line[MAX_LINE];
        int total = 0, broken = 0;
        while (fgets(line, sizeof(line), f)) {
            char name[MAX_NAME];
            if (sscanf(line, "%127s", name) == 1) {
                total++;
                if (verify_package_files(name) != 0) broken++;
            }
        }
        fclose(f);
        printf("\nChecked: %d, Broken: %d\n", total, broken);
        return broken > 0 ? 1 : 0;
    }
    return verify_package_files(argv[2]);
}

static int cmd_verify(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm verify <pkg>\n"); return 1; }
    return verify_package_files(argv[2]);
}

static int cmd_history(int argc, char **argv) {
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f) { printf("(no history)\n"); return 0; }
    char line[MAX_LINE];
    int count = 0;
    int limit = (argc >= 3) ? atoi(argv[2]) : 50;
    char lines[MAX_HISTORY][MAX_LINE];
    while (fgets(line, sizeof(line), f) && count < MAX_HISTORY) {
        SAFE_COPY(lines[count], line);
        count++;
    }
    fclose(f);
    int start = count > limit ? count - limit : 0;
    printf("%-8s %-30s %-15s %-20s %s\n", "ACTION", "PACKAGE", "VERSION", "TIME", "STATUS");
    printf("%-8s %-30s %-15s %-20s %s\n", "------", "-------", "-------", "----", "------");
    for (int i = start; i < count; i++) {
        char action[16], name[MAX_NAME], version[MAX_VERSION], date[24], status[8];
        if (sscanf(lines[i], "%15s %127s %63s %23[^\n] %7s", action, name, version, date, status) >= 4)
            printf("%-8s %-30s %-15s %-20s %s\n", action, name, version[0] ? version : "-", date, status);
    }
    return 0;
}

static int cmd_stats(void) {
    int pkg_count = db_count();
    int rc = repo_count();
    int snap_count = count_snapshots();
    long total_size = db_total_size();
    int lock_count = 0;
    FILE *lf = fopen(LOCK_FILE, "r");
    if (lf) { char l[MAX_LINE]; while (fgets(l, sizeof(l), lf)) lock_count++; fclose(lf); }
    long cache_size = 0;
    DIR *cd = opendir(CACHE_DIR);
    if (cd) {
        struct dirent *e;
        while ((e = readdir(cd))) {
            if (e->d_name[0] == '.') continue;
            char p[MAX_PATH];
            SAFE_SNPRINTF(p, "%s/%s", CACHE_DIR, e->d_name);
            cache_size += file_size(p);
        }
        closedir(cd);
    }
    printf("OkraLinux Package Manager v%s - Statistics\n", OKPM_VERSION);
    printf("===========================================\n");
    printf("Installed packages:  %d\n", pkg_count);
    printf("Configured sources:  %d\n", rc);
    printf("Locked packages:     %d\n", lock_count);
    printf("Snapshots:            %d\n", snap_count);
    printf("Total install size:   "); print_size_human(total_size); printf("\n");
    printf("Cache size:           "); print_size_human(cache_size); printf("\n");
    printf("Store directory:      %s\n", STORE_DIR);
    printf("Database:             %s\n", DB_FILE);
    printf("Cache directory:      %s\n", CACHE_DIR);
    return 0;
}

static int cmd_download(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm download <pkg> [pkg...]\n"); return 1; }
    int failures = 0;
    for (int i = 2; i < argc; i++) {
        char pkg_file[MAX_PATH];
        if (download_package(argv[i], pkg_file) != 0) {
            fprintf(stderr, "Error: cannot find %s\n", argv[i]);
            failures++;
        } else printf("  downloaded: %s -> %s\n", argv[i], pkg_file);
    }
    return failures > 0 ? 1 : 0;
}

static int cmd_provides(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm provides <file>\n"); return 1; }
    int found = find_provider(argv[2]);
    if (!found) printf("No package provides '%s'\n", argv[2]);
    return 0;
}

static int cmd_reinstall(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm reinstall <pkg>\n"); return 1; }
    return reinstall_package(argv[2]);
}

static int cmd_group(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm group <add|list|install|member> ...\n"); return 1; }
    if (strcmp(argv[2], "add") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: okpm group add <name> [desc]\n"); return 1; }
        char *desc = (argc >= 5) ? argv[4] : "";
        group_add(argv[3], desc);
        printf("OkraLinux: group '%s' added\n", argv[3]);
        return 0;
    }
    if (strcmp(argv[2], "list") == 0) {
        FILE *f = fopen(GROUP_FILE, "r");
        if (!f) return -1;
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) printf("  %s", line);
        fclose(f);
        return 0;
    }
    if (strcmp(argv[2], "install") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: okpm group install <name>\n"); return 1; }
        return group_install(argv[3]);
    }
    if (strcmp(argv[2], "member") == 0) {
        if (argc < 5) { fprintf(stderr, "Usage: okpm group member <add|list> <group> [pkg]\n"); return 1; }
        if (strcmp(argv[3], "add") == 0) {
            if (argc < 6) { fprintf(stderr, "Usage: okpm group member add <group> <pkg>\n"); return 1; }
            group_add_member(argv[4], argv[5]);
            printf("OkraLinux: added %s to group %s\n", argv[5], argv[4]);
            return 0;
        }
        if (strcmp(argv[3], "list") == 0) {
            group_list_members(argv[4]);
            return 0;
        }
    }
    fprintf(stderr, "Unknown group command: %s\n", argv[2]);
    return 1;
}

static int cmd_export(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm export <file>\n"); return 1; }
    return export_installed(argv[2]);
}

static int cmd_import(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm import <file>\n"); return 1; }
    return import_installed(argv[2]);
}

static void print_help(void) {
    printf("okpm v%s - OkraLinux Package Manager\n\n", OKPM_VERSION);
    printf("Usage: okpm [options] <command> [args]\n\n");
    printf("Options:\n");
    printf("  --yes          Skip confirmation prompts\n");
    printf("  --verbose      Verbose output\n");
    printf("  --no-verify    Skip checksum/signature verification\n");
    printf("  --no-hooks     Skip hook execution\n");
    printf("  --nodeps       Skip dependency resolution\n");
    printf("  --download-only  Download only, don't install\n");
    printf("  --force        Force operation\n\n");
    printf("Package Commands:\n");
    printf("  install <pkg>...        Install package(s)\n");
    printf("  remove <pkg>...         Remove package(s)\n");
    printf("  purge <pkg>...           Remove all versions\n");
    printf("  reinstall <pkg>          Reinstall package\n");
    printf("  upgrade [pkg]            Upgrade package(s)\n");
    printf("  update                   Check for updates\n");
    printf("  rollback                 Rollback to previous state\n");
    printf("  autoremove               Remove orphan packages\n\n");
    printf("Query Commands:\n");
    printf("  list [--installed|--upgradable|--repos|--locks|--snapshots|--orphans|--groups]\n");
    printf("  search <keyword>         Search packages\n");
    printf("  info <pkg>               Show package info\n");
    printf("  files <pkg>              List files owned by package\n");
    printf("  depends <pkg> [depth]    Show dependency tree\n");
    printf("  rdepends <pkg> [depth]   Show reverse dependency tree\n");
    printf("  provides <file>           Find package providing a file\n\n");
    printf("Source Commands:\n");
    printf("  add-source <url> [pri] [type] [arch]  Add repository\n");
    printf("  remove-source <url>                   Remove repository\n\n");
    printf("Build Commands:\n");
    printf("  build <dir>              Build .okra package\n\n");
    printf("Version Control:\n");
    printf("  lock <pkg> [version]     Lock package version\n");
    printf("  unlock <pkg>             Unlock package\n\n");
    printf("Group Commands:\n");
    printf("  group add <name> [desc]  Create package group\n");
    printf("  group install <name>     Install all packages in group\n");
    printf("  group member add <grp> <pkg>  Add package to group\n");
    printf("  group member list <grp>  List group members\n\n");
    printf("Maintenance:\n");
    printf("  check [pkg]              Verify package integrity\n");
    printf("  verify <pkg>             Verify package files\n");
    printf("  clean                    Clean temp/cache/old snapshots\n");
    printf("  stats                    Show statistics\n");
    printf("  history [n]              Show operation history\n");
    printf("  export <file>            Export installed list\n");
    printf("  import <file>            Import and install from list\n");
    printf("  download <pkg>...         Download only\n");
    printf("  help                     Show this help\n");
}

int main(int argc, char **argv) {
    int cmd_start = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) { g_ctx.yes = 1; cmd_start++; }
        else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) { g_ctx.verbose = 1; cmd_start++; }
        else if (strcmp(argv[i], "--no-verify") == 0) { g_ctx.no_verify = 1; cmd_start++; }
        else if (strcmp(argv[i], "--no-hooks") == 0) { g_ctx.no_hooks = 1; cmd_start++; }
        else if (strcmp(argv[i], "--nodeps") == 0) { g_ctx.nodeps = 1; cmd_start++; }
        else if (strcmp(argv[i], "--download-only") == 0) { g_ctx.download_only = 1; cmd_start++; }
        else if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) { g_ctx.force = 1; cmd_start++; }
        else break;
    }
    if (cmd_start >= argc) { print_help(); return 1; }
    db_init();
    load_config();
    if (acquire_lock() != 0) return 1;
    char *cmd = argv[cmd_start];
    int eargc = argc - cmd_start + 1;
    char **eargv = argv + cmd_start - 1;
    int ret;
    if (strcmp(cmd, "install") == 0) ret = cmd_install(eargc, eargv);
    else if (strcmp(cmd, "remove") == 0) ret = cmd_remove(eargc, eargv);
    else if (strcmp(cmd, "purge") == 0) ret = cmd_purge(eargc, eargv);
    else if (strcmp(cmd, "reinstall") == 0) ret = cmd_reinstall(eargc, eargv);
    else if (strcmp(cmd, "update") == 0) ret = cmd_update();
    else if (strcmp(cmd, "upgrade") == 0) ret = cmd_upgrade(eargc, eargv);
    else if (strcmp(cmd, "rollback") == 0) ret = cmd_rollback();
    else if (strcmp(cmd, "autoremove") == 0) ret = cmd_autoremove();
    else if (strcmp(cmd, "list") == 0) ret = cmd_list(eargc, eargv);
    else if (strcmp(cmd, "search") == 0) ret = cmd_search(eargc, eargv);
    else if (strcmp(cmd, "info") == 0) ret = cmd_info(eargc, eargv);
    else if (strcmp(cmd, "files") == 0) ret = cmd_files(eargc, eargv);
    else if (strcmp(cmd, "depends") == 0) ret = cmd_depends(eargc, eargv);
    else if (strcmp(cmd, "rdepends") == 0) ret = cmd_rdepends(eargc, eargv);
    else if (strcmp(cmd, "provides") == 0) ret = cmd_provides(eargc, eargv);
    else if (strcmp(cmd, "add-source") == 0) ret = cmd_add_source(eargc, eargv);
    else if (strcmp(cmd, "remove-source") == 0) ret = cmd_remove_source(eargc, eargv);
    else if (strcmp(cmd, "build") == 0) ret = cmd_build(eargc, eargv);
    else if (strcmp(cmd, "lock") == 0) ret = cmd_lock(eargc, eargv);
    else if (strcmp(cmd, "unlock") == 0) ret = cmd_unlock(eargc, eargv);
    else if (strcmp(cmd, "group") == 0) ret = cmd_group(eargc, eargv);
    else if (strcmp(cmd, "check") == 0) ret = cmd_check(eargc, eargv);
    else if (strcmp(cmd, "verify") == 0) ret = cmd_verify(eargc, eargv);
    else if (strcmp(cmd, "clean") == 0) ret = cmd_clean();
    else if (strcmp(cmd, "stats") == 0) ret = cmd_stats();
    else if (strcmp(cmd, "history") == 0) ret = cmd_history(eargc, eargv);
    else if (strcmp(cmd, "export") == 0) ret = cmd_export(eargc, eargv);
    else if (strcmp(cmd, "import") == 0) ret = cmd_import(eargc, eargv);
    else if (strcmp(cmd, "download") == 0) ret = cmd_download(eargc, eargv);
    else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) { print_help(); ret = 0; }
    else if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) { printf("okpm v%s\n", OKPM_VERSION); ret = 0; }
    else { fprintf(stderr, "Unknown command: %s\n\n", cmd); print_help(); ret = 1; }
    release_lock();
    return ret;
}