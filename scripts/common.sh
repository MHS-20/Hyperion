#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VM_DIR="$PROJECT_DIR/vm"
DISK_IMG="$VM_DIR/hyperion-vm.qcow2"
CLOUD_IMAGE="$VM_DIR/noble-server-cloudimg-amd64.img"
CLOUD_IMAGE_URL="https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img"
SEED_IMG="$VM_DIR/seed.img"
SSH_PORT=2222
SSH_KEY="$VM_DIR/vm_ed25519"
DISK_SIZE="20G"
VM_LOG="$VM_DIR/vm.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info()  { echo -e "${CYAN}[*]${NC} $*"; }
log_ok()    { echo -e "${GREEN}[+]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
log_err()   { echo -e "${RED}[!]${NC} $*"; }

die() { log_err "$@"; exit 1; }

download_cloud_image() {
    if [[ -f "$CLOUD_IMAGE" ]]; then
        log_info "Cloud image already downloaded: $CLOUD_IMAGE"
        return
    fi

    log_info "Downloading Ubuntu 24.04 cloud image (~650 MB)..."
    mkdir -p "$VM_DIR"
    wget -nc -O "$CLOUD_IMAGE" "$CLOUD_IMAGE_URL" || {
        rm -f "$CLOUD_IMAGE"
        die "Failed to download cloud image"
    }
    log_ok "Cloud image downloaded"
}

check_deps() {
    local missing=()
    for dep in qemu-system-x86_64 qemu-img; do
        command -v "$dep" &>/dev/null || missing+=("$dep")
    done
    if ! command -v cloud-localds &>/dev/null && ! command -v genisoimage &>/dev/null && ! command -v mkisofs &>/dev/null; then
        missing+=("cloud-localds or genisoimage")
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_err "Missing dependencies: ${missing[*]}"
        echo "  Ubuntu/Debian: sudo apt install qemu-system-x86 qemu-utils cloud-image-utils genisoimage openssh-client rsync"
        echo "  Fedora/RHEL:   sudo dnf install qemu-kvm qemu-img cloud-utils genisoimage openssh-clients rsync"
        echo "  Arch:          sudo pacman -S qemu-desktop cloud-utils cdrtools openssh rsync"
        return 1
    fi
    return 0
}

check_nested_kvm() {
    log_info "Checking nested KVM..."

    [[ -e /dev/kvm ]] || die "KVM not available. Enable virtualization in BIOS or load kvm module."

    if [[ ! -r /sys/module/kvm_intel/parameters/nested ]]; then
        if [[ -r /sys/module/kvm_amd/parameters/nested ]]; then
            die "AMD SVM detected. Use: echo 1 | sudo tee /sys/module/kvm_amd/parameters/nested"
        else
            die "Cannot find KVM module parameters. Is VT-x/AMD-V present?"
        fi
    fi

    local nested
    nested=$(cat /sys/module/kvm_intel/parameters/nested)

    if [[ "$nested" != "Y" && "$nested" != "1" ]]; then
        log_info "Enabling nested KVM..."
        sudo modprobe -r kvm_intel 2>/dev/null || true
        sudo modprobe kvm_intel nested=1
        echo "options kvm_intel nested=1" | sudo tee /etc/modprobe.d/kvm-intel.conf > /dev/null
        nested=$(cat /sys/module/kvm_intel/parameters/nested)
    fi

    if [[ "$nested" == "Y" || "$nested" == "1" ]]; then
        log_ok "Nested KVM enabled (nested=$nested)"
    else
        die "Failed to enable nested KVM (nested=$nested)"
    fi
}

generate_ssh_key() {
    mkdir -p "$VM_DIR"
    if [[ ! -f "$SSH_KEY" ]]; then
        log_info "Generating SSH key for VM..."
        ssh-keygen -t ed25519 -f "$SSH_KEY" -N "" -C "hyperion-vm" > /dev/null 2>&1
        log_ok "SSH key generated: $SSH_KEY"
    else
        log_info "SSH key exists: $SSH_KEY"
    fi
}

ssh_cmd() {
    ssh -q \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o IdentitiesOnly=yes \
        -i "$SSH_KEY" \
        -p "$SSH_PORT" \
        ubuntu@localhost \
        "$@"
}

scp_cmd() {
    scp -q \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o IdentitiesOnly=yes \
        -i "$SSH_KEY" \
        -P "$SSH_PORT" \
        "$@"
}

vm_is_running() {
    pgrep -f "qemu-system.*hyperion-vm" > /dev/null 2>&1
}

wait_for_ssh() {
    local attempts=0
    while ! ssh_cmd "echo ok" 2>/dev/null; do
        sleep 2
        ((attempts++))
        if [[ $attempts -gt 90 ]]; then
            die "Timed out waiting for SSH on port ${SSH_PORT}"
        fi
    done
    log_ok "SSH available on port ${SSH_PORT}"
}
