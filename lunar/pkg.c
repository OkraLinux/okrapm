#include "okpm.h"

int extract_okra(const char *pkg_file, const char *dest) {
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

int run_script(const char *script_path) {
    if (!file_exists(script_path)) return 0;
    char esc[MAX_PATH * 2];
    shell_escape(script_path, esc, sizeof(esc));
    char cmd[MAX_PATH * 3];
    SAFE_SNPRINTF(cmd, "chmod +x %s 2>/dev/null && %s 2>&1", esc, esc);
    return system(cmd);
}

int find_meta_in_dir(const char *dir, char *out_meta, char *out_base) {
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

int check_file_conflicts(const char *install_dir) {
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

int check_conflicts(const PkgEntry *pkg) {
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

void create_symlinks(const char *install_dir, const char *subdir, const char *target) {
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

void remove_symlinks(const char *install_dir, const char *subdir, const char *target) {
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

int find_okra_local(const char *name, char *out) {
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

int download_package(const char *name, char *out_file) {
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

int resolve_deps(const char *name, int depth, int *resolved_count,
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

char *create_snapshot(void) {
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

int switch_snapshot(const char *snap_id) {
    char snap_path[MAX_PATH];
    SAFE_SNPRINTF(snap_path, "%s/%s", SNAP_DIR, snap_id);
    if (!dir_exists(snap_path)) return -1;
    unlink(CURRENT_LINK);
    symlink(snap_path, CURRENT_LINK);
    return 0;
}

int count_snapshots(void) {
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

int cleanup_old_snapshots(int keep) {
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

int rollback_snapshot(void) {
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

int install_package(const char *pkg_file) {
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

int remove_package(const char *name) {
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

int reinstall_package(const char *name) {
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

int find_orphans(char orphans[][MAX_NAME], int *count) {
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

int check_reverse_deps(const char *name) {
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

int verify_package_files(const char *name) {
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

int export_installed(const char *out_file) {
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

int import_installed(const char *in_file) {
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

int render_dep_tree(const char *name, int depth, int max_depth,
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

int render_rdep_tree(const char *name, int depth, int max_depth,
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

int find_provider(const char *query) {
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

int group_install(const char *group) {
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
