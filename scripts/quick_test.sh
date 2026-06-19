#!/bin/bash
# quick_test.sh — fast build+test+log capture loop for Hyperion
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/test_vm.sh" 2>/dev/null || {
    source "$SCRIPT_DIR/common.sh"
    # start_vm lives in test_vm.sh; define it here as fallback
}

MODE="${1:-go}"

usage() {
    echo "Usage: $0 [go|stop|ssh|logs]"
    echo "  go      Start VM, copy src, build, test, capture logs"
    echo "  stop    Stop the VM"
    echo "  ssh     Interactive SSH"
    echo "  logs    Tail host-side VM console log"
    exit 1
}

do_test() {
    echo ""
    echo "=============================================="
    echo " Hyperion Quick Test"
    echo "=============================================="
    echo ""

    # 1. Ensure VM is running
    start_vm

    # 2. Copy latest sources
    log_info "Copying sources to VM..."
    ssh_cmd "sudo mkdir -p /opt/hyperion && sudo chown ubuntu:ubuntu /opt/hyperion"
    rsync -az \
        -e "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -i $SSH_KEY -p $SSH_PORT" \
        --exclude='*.o' --exclude='*.ko' --exclude='*.mod*' \
        --exclude='.cache' --exclude='modules.order' --exclude='Module.symvers' \
        --exclude='vm/' --exclude='scripts/' --exclude='docs/' \
        --exclude='.git' --exclude='*.png' \
        "$PROJECT_DIR/" "ubuntu@localhost:/opt/hyperion/" 2>&1 | tail -3
    log_ok "Sources copied"

    # 3. Build inside VM
    log_info "Building inside VM..."
    ssh_cmd 'bash -s' << 'BUILD'
set -e
echo "=== Building kernel module ==="
cd /opt/hyperion/module && make clean 2>&1 | tail -3 && make 2>&1
echo "=== Building user program ==="
cd /opt/hyperion/user && make clean 2>&1 | tail -1 && make 2>&1
echo "=== Build complete ==="
BUILD
    log_ok "Build succeeded"

    # 4. Run test with log capture
    echo ""
    log_info "Running Hyperion test..."
    echo ""

    ssh_cmd 'bash -s' << 'TEST'
set -euo pipefail

# Send ALL kernel messages to console (captured by QEMU)
echo 8 | sudo tee /proc/sys/kernel/printk > /dev/null

# Clean slate
sudo rmmod hyperion 2>/dev/null || true
sudo dmesg -C > /dev/null 2>&1

echo "--- Loading module ---"
sudo insmod /opt/hyperion/module/hyperion.ko
echo "insmod: OK ($(lsmod | grep -c hyperion) instance loaded)"

echo ""
echo "--- Running user program (5s timeout) ---"
echo "" | sudo timeout 5 /opt/hyperion/user/hyperion-user 2>&1 || true

sleep 2

echo ""
echo "--- KERNEL LOG (dmesg) ---"
sudo dmesg -c 2>/dev/null || sudo dmesg
echo "--- END LOG ---"

echo ""
echo "--- ANALYSIS ---"
KLOG=$(sudo dmesg 2>/dev/null || echo "")

if echo "$KLOG" | grep -qiE "(Kernel panic|BUG:|Oops:|general protection fault|RIP:.*hyperion|unable to handle)"; then
    echo "[CRITICAL] Kernel panic/OOPS detected!"
elif echo "$KLOG" | grep -q "VMLAUNCH error"; then
    echo "[ERROR] VMLAUNCH_FAILED"
    echo "$KLOG" | grep "VMLAUNCH error"
elif echo "$KLOG" | grep -q "VMLAUNCH succeeded"; then
    echo "[OK] VMLAUNCH succeeded"
elif echo "$KLOG" | grep -q "VMX initialization failed"; then
    echo "[FAIL] VMX_INIT_FAILED"
elif echo "$KLOG" | grep -q "VMX initialized successfully"; then
    echo "[OK] VMX init appears successful"
else
    echo "[UNKNOWN] Check dmesg output above"
fi

echo ""
echo "--- CLEANUP ---"
sudo rmmod hyperion 2>/dev/null && echo "rmmod: OK" || echo "rmmod: N/A (already unloaded or in use)"
TEST

    echo ""
    log_info "VM is still running. Commands:"
    echo "  ${CYAN}$0 ssh${NC}     - interactive shell"
    echo "  ${CYAN}$0 logs${NC}    - tail VM console log"
    echo "  ${CYAN}$0 stop${NC}    - shut down VM"
}

# ---- Dispatch ----
case "${MODE:-}" in
    go)     do_test ;;
    stop)   stop_vm ;;
    ssh)    start_vm; log_info "Connecting (Ctrl+D to exit)..."; ssh_cmd ;;
    logs)   tail -f "$VM_LOG" ;;
    *)      usage ;;
esac
