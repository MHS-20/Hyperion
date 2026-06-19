#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info()  { echo -e "${CYAN}[*]${NC} $*"; }
log_ok()    { echo -e "${GREEN}[+]${NC} $*"; }
log_warn()  { echo -e "${RED}[!]${NC} $*"; }

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=true
    log_info "Dry run — nothing will be removed"
fi

remove() {
    if $DRY_RUN; then
        echo "  would remove: $*"
    else
        rm -rf "$@"
    fi
}

# --- Kill any running Hyperion test VM ---
if ! $DRY_RUN && pgrep -f "qemu-system.*hyperion-vm" > /dev/null 2>&1; then
    log_warn "Hyperion test VM is running — killing it"
    pkill -f "qemu-system.*hyperion-vm" || true
    sleep 1
fi

# --- Build artifacts via make clean ---
log_info "Module build artifacts"
if [[ -f "$PROJECT_DIR/module/Makefile" ]]; then
    if $DRY_RUN; then
        echo "  would run: make -C $PROJECT_DIR/module clean"
    else
        make -C "$PROJECT_DIR/module" clean 2>/dev/null || true
    fi
fi

log_info "User build artifacts"
if [[ -f "$PROJECT_DIR/user/Makefile" ]]; then
    if $DRY_RUN; then
        echo "  would run: make -C $PROJECT_DIR/user clean"
    else
        make -C "$PROJECT_DIR/user" clean 2>/dev/null || true
    fi
fi

# --- clangd index cache ---
log_info "clangd cache"
remove "$PROJECT_DIR/module/.cache"

# --- VM artifacts ---
log_info "VM disk images, SSH keys, seed ISO, logs"
remove "$PROJECT_DIR/vm"

# --- compile_commands.json ---
if [[ -f "$PROJECT_DIR/compile_commands.json" ]]; then
    log_info "compile_commands.json"
    remove "$PROJECT_DIR/compile_commands.json"
fi

# --- Stray kernel build artifacts that make clean may miss ---
log_info "Stray build artifacts"
remove "$PROJECT_DIR/module/.tmp_versions" 2>/dev/null || true

if ! $DRY_RUN; then
    log_ok "Cleanup complete"
else
    log_ok "Dry run complete"
fi
