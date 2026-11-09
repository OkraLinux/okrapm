#include "okpm.h"

int cmd_install(int argc, char **argv) {
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

int cmd_remove(int argc, char **argv) {
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

int cmd_purge(int argc, char **argv) {
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

int cmd_update(void) {
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

int cmd_upgrade(int argc, char **argv) {
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

int cmd_rollback(void) { return rollback_snapshot(); }

int cmd_list(int argc, char **argv) {
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

int cmd_search(int argc, char **argv) {
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

int cmd_info(int argc, char **argv) {
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

int cmd_files(int argc, char **argv) {
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

int cmd_add_source(int argc, char **argv) {
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

int cmd_remove_source(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm remove-source <url>\n"); return 1; }
    if (repo_remove(argv[2]) != 0) { printf("OkraLinux: source not found: %s\n", argv[2]); return 0; }
    printf("OkraLinux: source removed: %s\n", argv[2]);
    log_write("remove-source", argv[2], "", 1);
    return 0;
}

int cmd_build(int argc, char **argv) {
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

int cmd_lock(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm lock <pkg> [version]\n"); return 1; }
    char *name = argv[2];
    char *version = (argc >= 4) ? argv[3] : "current";
    PkgEntry pkg;
    if (db_get_entry(name, &pkg) != 0) { fprintf(stderr, "Error: %s not installed\n", name); return -1; }
    if (lock_add(name, version) != 0) { fprintf(stderr, "Error: failed to lock %s\n", name); return -1; }
    printf("OkraLinux: locked %s at %s\n", name, version);
    return 0;
}

int cmd_unlock(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm unlock <pkg>\n"); return 1; }
    if (lock_remove(argv[2]) != 0) { fprintf(stderr, "Error: failed to unlock %s\n", argv[2]); return -1; }
    printf("OkraLinux: unlocked %s\n", argv[2]);
    return 0;
}

int cmd_clean(void) {
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

int cmd_autoremove(void) {
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

int cmd_depends(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm depends <pkg> [depth]\n"); return 1; }
    int max_depth = (argc >= 4) ? atoi(argv[3]) : 5;
    char visited[MAX_VISITED][MAX_NAME];
    memset(visited, 0, sizeof(visited));
    int visited_count = 0;
    render_dep_tree(argv[2], 0, max_depth, (const char **)visited, &visited_count);
    return 0;
}

int cmd_rdepends(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm rdepends <pkg> [depth]\n"); return 1; }
    int max_depth = (argc >= 4) ? atoi(argv[3]) : 3;
    printf("Reverse dependencies of %s:\n", argv[2]);
    char visited[MAX_VISITED][MAX_NAME];
    memset(visited, 0, sizeof(visited));
    int visited_count = 0;
    render_rdep_tree(argv[2], 0, max_depth, (const char **)visited, &visited_count);
    return 0;
}

int cmd_check(int argc, char **argv) {
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

int cmd_verify(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm verify <pkg>\n"); return 1; }
    return verify_package_files(argv[2]);
}

int cmd_history(int argc, char **argv) {
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

int cmd_stats(void) {
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

int cmd_download(int argc, char **argv) {
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

int cmd_provides(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm provides <file>\n"); return 1; }
    int found = find_provider(argv[2]);
    if (!found) printf("No package provides '%s'\n", argv[2]);
    return 0;
}

int cmd_reinstall(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm reinstall <pkg>\n"); return 1; }
    return reinstall_package(argv[2]);
}

int cmd_group(int argc, char **argv) {
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

int cmd_export(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm export <file>\n"); return 1; }
    return export_installed(argv[2]);
}

int cmd_import(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: okpm import <file>\n"); return 1; }
    return import_installed(argv[2]);
}

void print_help(void) {
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
