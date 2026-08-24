#!/bin/sh
# oaatools-common.sh - Shared helper library for OAA tools
# Compatible with BusyBox ash / bash / dash (POSIX sh)

OAA_COLOR_RED='\033[1;31m'
OAA_COLOR_GRN='\033[1;32m'
OAA_COLOR_YEL='\033[1;33m'
OAA_COLOR_CYN='\033[1;36m'
OAA_COLOR_RST='\033[0m'

oaa_info()  { printf "${OAA_COLOR_GRN}::${OAA_COLOR_RST} %s\n" "$*"; }
oaa_warn()  { printf "${OAA_COLOR_YEL}!!${OAA_COLOR_RST} %s\n" "$*" >&2; }
oaa_error() { printf "${OAA_COLOR_RED}xx${OAA_COLOR_RST} %s\n" "$*" >&2; }
oaa_title() { printf "${OAA_COLOR_CYN}##${OAA_COLOR_RST} %s\n" "$*"; }

oaa_die() {
    oaa_error "$*"
    exit 1
}

# oaa_get_meta_field <meta.yaml-content> <key>
# Returns the first value for simple "key: value" lines.
oaa_get_meta_field() {
    awk -v key="$2" '
        BEGIN { found=0 }
        /^[[:space:]]*#/ { next }
        {
            line=$0
            sub(/^[[:space:]]+/, "", line)
            if (substr(line, 1, length(key)+1) == key ":") {
                val=substr(line, length(key)+2)
                gsub(/^[[:space:]]+/, "", val)
                gsub(/^["'\''"]/, "", val)
                gsub(/["'\''"][[:space:]]*$/, "", val)
                print val
                found=1
                exit
            }
        }
        END { if (!found) exit 1 }
    ' <<EOF
$1
EOF
}

# oaa_get_meta_list <meta.yaml-content> <key>
# Returns the values for a YAML list block (key:\n  - item\n  - item)
oaa_get_meta_list() {
    awk -v key="$2" '
        BEGIN { in_block=0 }
        /^[[:space:]]*#/ { next }
        {
            line=$0
            sub(/^[[:space:]]+/, "", line)
            if (substr(line, 1, length(key)+1) == key ":") {
                in_block=1
                next
            }
            if (in_block) {
                if (substr(line, 1, 2) == "- ") {
                    val=substr(line, 3)
                    gsub(/^["'\''"]/, "", val)
                    gsub(/["'\''"][[:space:]]*$/, "", val)
                    print val
                    next
                }
                if (line != "" && substr(line, 1, 2) != "- ") {
                    in_block=0
                }
            }
        }
    ' <<EOF
$1
EOF
}

# oaa_extract_meta <oaa-file>
# Dumps the meta.yaml content from an .oaa archive to stdout.
oaa_extract_meta() {
    oaa_file="$1"
    [ -f "$oaa_file" ] || return 1
    for attempt_cmd in \
        "tar --zstd -xf \"$oaa_file\" -O ./meta.yaml 2>/dev/null" \
        "tar --zstd -xf \"$oaa_file\" -O meta.yaml 2>/dev/null" \
        "tar -xf \"$oaa_file\" -O ./meta.yaml 2>/dev/null" \
        "tar -xf \"$oaa_file\" -O meta.yaml 2>/dev/null" \
        "tar -xz -xf \"$oaa_file\" -O ./meta.yaml 2>/dev/null" \
        "tar -xz -xf \"$oaa_file\" -O meta.yaml 2>/dev/null" \
        "tar --zstd --wildcards -xf \"$oaa_file\" -O '*meta.yaml' 2>/dev/null" \
        "tar --wildcards -xf \"$oaa_file\" -O '*meta.yaml' 2>/dev/null"; do
        meta_out=$(eval "$attempt_cmd")
        if [ -n "$meta_out" ]; then
            printf '%s\n' "$meta_out"
            return 0
        fi
    done
    return 1
}

# oaa_sha256sum <file>
# Outputs the SHA256 hash of the given file. Returns nonzero on failure.
oaa_sha256sum() {
    file="$1"
    [ -f "$file" ] || return 1
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$file" | awk '{print $1}'
    elif command -v sha256 >/dev/null 2>&1; then
        sha256 "$file" | awk '{print $NF}'
    else
        oaa_error "no sha256/sha256sum utility available"
        return 1
    fi
}

# oaa_pick_compression: print compression flag for tar based on file ext or content
oaa_pick_compression() {
    case "$1" in
        *.tar.zst|*.tar.zstd|*.oaa) printf '%s' "--zstd" ;;
        *.tar.gz|*.tgz)             printf '%s' "-z" ;;
        *.tar.xz|*.txz)             printf '%s' "-J" ;;
        *)                          printf '%s' "--zstd" ;;
    esac
}

# oaa_human_size <bytes>
oaa_human_size() {
    bytes="$1"
    case "$bytes" in
        ''|*[!0-9]*) printf '%s' "0 B"; return ;;
    esac
    if [ "$bytes" -lt 1024 ]; then
        printf '%s B' "$bytes"
    elif [ "$bytes" -lt 1048576 ]; then
        awk -v n="$bytes" 'BEGIN { printf "%.1f KB", n/1024 }'
    elif [ "$bytes" -lt 1073741824 ]; then
        awk -v n="$bytes" 'BEGIN { printf "%.1f MB", n/1048576 }'
    else
        awk -v n="$bytes" 'BEGIN { printf "%.2f GB", n/1073741824 }'
    fi
}
