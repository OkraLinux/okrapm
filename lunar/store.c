#include "okpm.h"

int parse_meta(const char *yaml_path, PkgEntry *pkg) {
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

void load_config(void) {
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

void compute_sha256(const char *path, char *out) {
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

int verify_sha256(const char *path, const char *expected) {
    if (g_ctx.no_verify || !expected[0]) return 0;
    char actual[MAX_HASH];
    compute_sha256(path, actual);
    if (actual[0] && strcmp(actual, expected) != 0) return -1;
    return 0;
}

int verify_signature(const char *pkg_file) {
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

void db_init(void) {
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

int db_is_installed(const char *name) {
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

int db_provides_installed(const char *dep) {
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

int db_get_entry(const char *name, PkgEntry *pkg) {
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

int db_add(const PkgEntry *pkg) {
    FILE *f = fopen(DB_FILE, "a");
    if (!f) return -1;
    fprintf(f, "%s %s %s\n", pkg->name, pkg->version, pkg->install_path);
    fclose(f);
    return 0;
}

int db_remove(const char *name) {
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

int db_count(void) {
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

long db_total_size(void) {
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

int is_locked(const char *name) {
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

int lock_add(const char *name, const char *version) {
    if (is_locked(name)) return 0;
    FILE *f = fopen(LOCK_FILE, "a");
    if (!f) return -1;
    fprintf(f, "%s %s\n", name, version);
    fclose(f);
    return 0;
}

int lock_remove(const char *name) {
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

int repo_add(const char *url, int priority, const char *type, const char *arch) {
    FILE *f = fopen(REPO_FILE, "a");
    if (!f) return -1;
    fprintf(f, "%s %d %s %s %d\n", url, priority, type, arch ? arch : "any", 1);
    fclose(f);
    return 0;
}

int repo_count(void) {
    FILE *f = fopen(REPO_FILE, "r");
    if (!f) return 0;
    int count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) count++;
    fclose(f);
    return count;
}

int repo_remove(const char *url) {
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

int group_add(const char *name, const char *desc) {
    FILE *f = fopen(GROUP_FILE, "a");
    if (!f) return -1;
    fprintf(f, "%s %s\n", name, desc ? desc : "");
    fclose(f);
    return 0;
}

int group_add_member(const char *group, const char *pkg) {
    char path[MAX_PATH];
    SAFE_SNPRINTF(path, "%s/%s.members", DB_DIR, group);
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    fprintf(f, "%s\n", pkg);
    fclose(f);
    return 0;
}

int group_list_members(const char *group) {
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

int hook_load_all(Hook *hooks, int *count) {
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

int hook_run(const char *trigger, const char *when, const char *pkg_name) {
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
