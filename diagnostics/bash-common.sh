#!/usr/bin/env bash
# Shared Windows bridge for the Git Bash entry points.
diagnostics_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

check_port() {
    if [[ ! $1 =~ ^[0-9]{1,5}$ ]] || (( 10#$1 < 1 || 10#$1 > 65535 )); then
        printf 'Port must be an integer from 1 to 65535.\n' >&2
        return 2
    fi
}

run_diagnostic_powershell() {
    local script=$1
    shift
    if ! command -v cygpath >/dev/null || ! command -v powershell.exe >/dev/null; then
        printf 'Run this script in Git Bash on Windows (cygpath and powershell.exe are required).\n' >&2
        return 1
    fi
    local windows_script
    windows_script=$(cygpath -aw "$diagnostics_dir/$script")
    # Paths are converted explicitly; preserve server/nickname arguments verbatim.
    export MSYS2_ARG_CONV_EXCL='*'
    exec powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "$windows_script" "$@"
}
