#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: bash start-clients.sh [server-ipv4 [port [name-a [name-b]]]]\nDefaults: 127.0.0.1 27020 LocalA LocalB. Close both games before a new run.\n'
}
if [[ ${1:-} == --help || ${1:-} == -h ]]; then usage; exit 0; fi
if (( $# > 4 )); then usage >&2; exit 2; fi
source "$(dirname -- "${BASH_SOURCE[0]}")/bash-common.sh"
server=${1:-127.0.0.1}
port=${2:-27020}
name_a=${3:-LocalA}
name_b=${4:-LocalB}
check_port "$port"
run_diagnostic_powershell start-clients.ps1 -ServerAddress "$server" -Port "$port" -NameA "$name_a" -NameB "$name_b"
