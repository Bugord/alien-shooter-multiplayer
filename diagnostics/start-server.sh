#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: bash start-server.sh [port]\nDefault: UDP 27020. The server stays in this terminal; Ctrl+C stops it.\n'
}
if [[ ${1:-} == --help || ${1:-} == -h ]]; then usage; exit 0; fi
if (( $# > 1 )); then usage >&2; exit 2; fi
source "$(dirname -- "${BASH_SOURCE[0]}")/bash-common.sh"
port=${1:-27020}
check_port "$port"
run_diagnostic_powershell start-server.ps1 -Port "$port"
