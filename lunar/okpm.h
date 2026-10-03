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

extern OkpmContext g_ctx;
extern int g_lock_fd;
extern char g_tmpdir[MAX_PATH];
extern int g_config_loaded;

char *trim(char *s);
int file_exists(const char *path);
int dir_exists(const char *path);
long file_size(const char *path);
void mkdir_p(const char *path);
int has_tool(const char *tool);
int shell_escape(const char *input, char *out, int outlen);
int safe_exec(const char **argv);
int safe_copy_file(const char *src, const char *dst);
int safe_remove_tree(const char *path);
void rotate_log(void);
void log_msg(const char *fmt, ...);
void log_write(const char *action, const char *name, const char *version, int success);
int acquire_lock(void);
void release_lock(void);
void cleanup_tmpdir(void);
int init_tmpdir(void);
void parse_list_item(char *line, char deps[][MAX_NAME], int *count, int max);
void print_size_human(long bytes);
int confirm_action(const char *prompt);
int has_visited(const char *visited[][MAX_NAME], int count, const char *name);
int parse_meta(const char *yaml_path, PkgEntry *pkg);
void load_config(void);
void compute_sha256(const char *path, char *out);
int verify_sha256(const char *path, const char *expected);
int verify_signature(const char *pkg_file);
void db_init(void);
int db_is_installed(const char *name);
int db_provides_installed(const char *dep);
int db_get_entry(const char *name, PkgEntry *pkg);
int db_add(const PkgEntry *pkg);
int db_remove(const char *name);
int db_count(void);
long db_total_size(void);
int is_locked(const char *name);
int lock_add(const char *name, const char *version);
int lock_remove(const char *name);
int repo_add(const char *url, int priority, const char *type, const char *arch);
int repo_count(void);
int repo_remove(const char *url);
int group_add(const char *name, const char *desc);
int group_add_member(const char *group, const char *pkg);
int group_list_members(const char *group);
int hook_load_all(Hook *hooks, int *count);
int hook_run(const char *trigger, const char *when, const char *pkg_name);
int extract_okra(const char *pkg_file, const char *dest);
int run_script(const char *script_path);
int find_meta_in_dir(const char *dir, char *out_meta, char *out_base);
int check_file_conflicts(const char *install_dir);
int check_conflicts(const PkgEntry *pkg);
void create_symlinks(const char *install_dir, const char *subdir, const char *target);
void remove_symlinks(const char *install_dir, const char *subdir, const char *target);
int find_okra_local(const char *name, char *out);
int download_package(const char *name, char *out_file);
int resolve_deps(const char *name, int depth, int *resolved_count, const char *visited[], int *visited_count);
char *create_snapshot(void);
int switch_snapshot(const char *snap_id);
int count_snapshots(void);
int cleanup_old_snapshots(int keep);
int rollback_snapshot(void);
int install_package(const char *pkg_file);
int remove_package(const char *name);
int reinstall_package(const char *name);
int find_orphans(char orphans[][MAX_NAME], int *count);
int check_reverse_deps(const char *name);
int verify_package_files(const char *name);
int export_installed(const char *out_file);
int import_installed(const char *in_file);
int render_dep_tree(const char *name, int depth, int max_depth, const char *visited[], int *visited_count);
int render_rdep_tree(const char *name, int depth, int max_depth, const char *visited[], int *visited_count);
int find_provider(const char *query);
int group_install(const char *group);
int cmd_install(int argc, char **argv);
int cmd_remove(int argc, char **argv);
int cmd_purge(int argc, char **argv);
int cmd_update(void);
int cmd_upgrade(int argc, char **argv);
int cmd_rollback(void);
int cmd_list(int argc, char **argv);
int cmd_search(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_files(int argc, char **argv);
int cmd_add_source(int argc, char **argv);
int cmd_remove_source(int argc, char **argv);
int cmd_build(int argc, char **argv);
int cmd_lock(int argc, char **argv);
int cmd_unlock(int argc, char **argv);
int cmd_clean(void);
int cmd_autoremove(void);
int cmd_depends(int argc, char **argv);
int cmd_rdepends(int argc, char **argv);
int cmd_check(int argc, char **argv);
int cmd_verify(int argc, char **argv);
int cmd_history(int argc, char **argv);
int cmd_stats(void);
int cmd_download(int argc, char **argv);
int cmd_provides(int argc, char **argv);
int cmd_reinstall(int argc, char **argv);
int cmd_group(int argc, char **argv);
int cmd_export(int argc, char **argv);
int cmd_import(int argc, char **argv);
void print_help(void);
int main(int argc, char **argv);
