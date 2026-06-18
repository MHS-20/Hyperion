#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

echo "=============================================="
echo " Hyperion VM Setup"
echo "=============================================="

check_deps
check_nested_kvm
generate_ssh_key
download_cloud_image

log_info "Creating cloud-init seed ISO..."

SSH_PUBKEY=$(cat "${SSH_KEY}.pub")

cat > "$VM_DIR/user-data" << CLOUDINIT
#cloud-config
hostname: hyperion-test
users:
  - name: ubuntu
    lock_passwd: true
    sudo: ALL=(ALL) NOPASSWD:ALL
    ssh_authorized_keys:
      - ${SSH_PUBKEY}
ssh_pwauth: false
package_update: true
package_upgrade: false
packages:
  - build-essential
  - openssh-server

runcmd:
  - apt-get install -y "linux-headers-\$(uname -r)" 2>&1 || apt-get install -y linux-headers-generic 2>&1
  - systemctl enable --now ssh
  - echo 'Hyperion test VM is ready' > /etc/motd
  - [ shutdown, -h, now ]
CLOUDINIT

cat > "$VM_DIR/meta-data" << 'META'
instance-id: hyperion-test-01
local-hostname: hyperion-test
META

rm -f "$SEED_IMG"

if command -v cloud-localds &>/dev/null; then
    cloud-localds "$SEED_IMG" "$VM_DIR/user-data" "$VM_DIR/meta-data"
elif command -v genisoimage &>/dev/null; then
    (cd "$VM_DIR" && genisoimage -output seed.img -volid cidata -joliet -rock \
        user-data meta-data) > /dev/null 2>&1
elif command -v mkisofs &>/dev/null; then
    (cd "$VM_DIR" && mkisofs -output seed.img -volid cidata -joliet -rock \
        user-data meta-data) > /dev/null 2>&1
else
    die "No ISO creation tool found (cloud-localds, genisoimage, or mkisofs)"
fi

log_ok "Seed ISO created"

log_info "Creating VM disk image (${DISK_SIZE})..."

if [[ -f "$DISK_IMG" ]]; then
    log_warn "Disk image already exists: $DISK_IMG"
    echo "  Remove and recreate: rm $DISK_IMG && bash scripts/setup_vm.sh"
else
    qemu-img create -f qcow2 -b "$CLOUD_IMAGE" -F qcow2 "$DISK_IMG" "$DISK_SIZE"
    qemu-img resize "$DISK_IMG" "$DISK_SIZE" > /dev/null 2>&1 || true
    log_ok "Disk image created"
fi

echo ""
log_info "Booting VM for initial cloud-init setup..."
log_info "Installing packages (build-essential, kernel headers) then shutting down..."
echo ""

qemu-system-x86_64 \
    -enable-kvm \
    -cpu host,+vmx \
    -m 4096 \
    -smp 2 \
    -drive file="$DISK_IMG",format=qcow2,if=virtio \
    -drive file="$SEED_IMG",format=raw,if=virtio \
    -nic user,model=virtio,hostfwd=tcp::${SSH_PORT}-:22 \
    -nographic \
    -no-reboot

echo ""
log_ok "VM initial setup complete. Cloud-init packages installed."
log_ok "VM disk ready for testing."
echo ""
log_info "Run the test suite:"
echo "  ${CYAN}bash scripts/test_vm.sh${NC}"
echo ""
log_info "Or start the VM for interactive debugging:"
echo "  ${CYAN}bash scripts/test_vm.sh start${NC}"
echo "  ${CYAN}bash scripts/test_vm.sh ssh${NC}"
