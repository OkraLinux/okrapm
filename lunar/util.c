#include "okpm.h"

char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    return s;
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return st.st_size;
}

void mkdir_p(const char *path) {
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

int has_tool(const char *tool) {
    char cmd[MAX_PATH];
    SAFE_SNPRINTF(cmd, "command -v '%s' >/dev/null 2>&1", tool);
    return system(cmd) == 0;
}

int shell_escape(const char *input, char *out, int outlen) {
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

int safe_exec(const char **argv) {
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

int safe_copy_file(const char *src, const char *dst) {
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

int safe_remove_tree(const char *path) {
    if (!path || !*path || strcmp(path, "/") == 0) return -1;
    if (strlen(path) < 2) return -1;
    char esc[MAX_PATH * 2];
    shell_escape(path, esc, sizeof(esc));
    char cmd[MAX_PATH * 3];
    SAFE_SNPRINTF(cmd, "rm -rf %s 2>/dev/null", esc);
    return system(cmd);
}

void rotate_log(void) {
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > LOG_MAX_SIZE) {
        char old[MAX_PATH];
        SAFE_SNPRINTF(old, "%s.old", LOG_FILE);
        rename(LOG_FILE, old);
    }
}

void log_msg(const char *fmt, ...) {
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

void log_write(const char *action, const char *name, const char *version, int success) {
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

int acquire_lock(void) {
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

void release_lock(void) {
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
        unlink(PID_FILE);
    }
}

void cleanup_tmpdir(void) {
    if (g_tmpdir[0]) safe_remove_tree(g_tmpdir);
}

int init_tmpdir(void) {
    SAFE_SNPRINTF(g_tmpdir, "%s/okpm_%d", TMP_BASE, (int)getpid());
    mkdir_p(g_tmpdir);
    atexit(cleanup_tmpdir);
    return 0;
}

void parse_list_item(char *line, char deps[][MAX_NAME], int *count, int max) {
    char *p = strchr(line, '-');
    if (!p) return;
    p++;
    char *item = trim(p);
    if (*item && *count < max) {
        SAFE_COPY(deps[*count], item);
        (*count)++;
    }
}

void print_size_human(long bytes) {
    if (bytes < 1024) printf("%ld B", bytes);
    else if (bytes < 1024 * 1024) printf("%.1f KB", bytes / 1024.0);
    else if (bytes < 1024 * 1024 * 1024) printf("%.1f MB", bytes / (1024.0 * 1024));
    else printf("%.1f GB", bytes / (1024.0 * 1024 * 1024));
}

int confirm_action(const char *prompt) {
    if (g_ctx.yes) return 1;
    printf("%s [y/N] ", prompt);
    fflush(stdout);
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    return buf[0] == 'y' || buf[0] == 'Y';
}

int has_visited(const char *visited[][MAX_NAME], int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(visited[i], name) == 0) return 1;
    }
    return 0;
}
