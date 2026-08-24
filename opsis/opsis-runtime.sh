#!/bin/sh
# opsis-runtime.sh - Standard runtime library for OkraLinux Package Standard Installation Script (OPSIS)
# Complies with POSIX sh / BusyBox ash

OPSIS_VERSION="1.0.0"

# Colors
CLR_RED='\033[1;31m'
CLR_GRN='\033[1;32m'
CLR_YEL='\033[1;33m'
CLR_BLU='\033[1;34m'
CLR_CYN='\033[1;36m'
CLR_RST='\033[0m'

opsis_log_info()  { printf "${CLR_GRN}[OPSIS:INFO]${CLR_RST} %s\n" "$*"; }
opsis_log_warn()  { printf "${CLR_YEL}[OPSIS:WARN]${CLR_RST} %s\n" "$*" >&2; }
opsis_log_error() { printf "${CLR_RED}[OPSIS:FAIL]${CLR_RST} %s\n" "$*" >&2; }
opsis_log_step()  { printf "${CLR_CYN}[OPSIS:STEP]${CLR_RST} %s\n" "$*"; }

opsis_die() {
    opsis_log_error "$*"
    exit 1
}

# Ensure destination root directory
OPSIS_SYSROOT="${OPSIS_SYSROOT:-/}"
[ -d "$OPSIS_SYSROOT" ] || opsis_die "Target sysroot does not exist: $OPSIS_SYSROOT"

# State and database tracking directory
OPSIS_DB_DIR="${OPSIS_SYSROOT}/var/lib/okrapm/db"
mkdir -p "$OPSIS_DB_DIR"

# ----------------- Lifecycle Helpers -----------------

opsis_check_user() {
    if [ "$(id -u)" -ne 0 ] && [ "$OPSIS_ALLOW_NONROOT" != "1" ]; then
        opsis_die "OPSIS installation requires root privileges."
    fi
}

opsis_check_disk_space() {
    req_kb="$1"
    [ -n "$req_kb" ] || return 0
    avail_kb=$(df -k "$OPSIS_SYSROOT" 2>/dev/null | awk 'NR==2 {print $4}')
    if [ -n "$avail_kb" ] && [ "$avail_kb" -lt "$req_kb" ]; then
        opsis_die "Insufficient disk space in $OPSIS_SYSROOT: required ${req_kb}KB, available ${avail_kb}KB"
    fi
}

opsis_install_file() {
    src="$1"
    dst="$2"
    mode="${3:-0755}"
    owner="${4:-0:0}"

    dst_path="${OPSIS_SYSROOT}/${dst#/}"
    dst_dir="$(dirname "$dst_path")"
    mkdir -p "$dst_dir" || opsis_die "Cannot create directory $dst_dir"

    cp -a "$src" "$dst_path" || opsis_die "Failed to install $src -> $dst_path"
    chmod "$mode" "$dst_path" 2>/dev/null || true
    chown "$owner" "$dst_path" 2>/dev/null || true
    opsis_log_info "  -> installed $dst"
}

opsis_install_dir() {
    src_dir="$1"
    dst_prefix="${2:-/}"
    
    [ -d "$src_dir" ] || return 0
    (
        cd "$src_dir" || exit 1
        find . -mindepth 1 | while read -r item; do
            clean_item="${item#./}"
            target_path="${OPSIS_SYSROOT}/${dst_prefix#/}/${clean_item}"
            if [ -d "$item" ]; then
                mkdir -p "$target_path"
            else
                mkdir -p "$(dirname "$target_path")"
                cp -a "$item" "$target_path"
            fi
        done
    )
}

opsis_update_ldconfig() {
    if [ -x "${OPSIS_SYSROOT}/usr/bin/ldconfig" ] || [ -x "${OPSIS_SYSROOT}/sbin/ldconfig" ]; then
        opsis_log_step "Updating dynamic linker run-time bindings (ldconfig)..."
        chroot "$OPSIS_SYSROOT" /usr/bin/ldconfig 2>/dev/null || ldconfig -r "$OPSIS_SYSROOT" 2>/dev/null || true
    fi
}

opsis_update_systemd_units() {
    if [ -d "${OPSIS_SYSROOT}/run/systemd/system" ] && command -v systemctl >/dev/null 2>&1; then
        opsis_log_step "Reloading systemd daemon..."
        systemctl daemon-reload 2>/dev/null || true
    fi
}

# ----------------- Package Record Tracker -----------------

opsis_record_installed() {
    pkg_name="$1"
    pkg_ver="$2"
    manifest_file="$3"

    pkg_dir="${OPSIS_DB_DIR}/${pkg_name}"
    mkdir -p "$pkg_dir"

    echo "$pkg_ver" > "$pkg_dir/version"
    date -u +"%Y-%m-%dT%H:%M:%SZ" > "$pkg_dir/installed_time"
    if [ -f "$manifest_file" ]; then
        cp "$manifest_file" "$pkg_dir/manifest"
    fi
    opsis_log_info "Recorded package $pkg_name-$pkg_ver in system database."
}
