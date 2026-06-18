#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

MODE="${1:-test}"

usage() {
    echo "Usage: $0 [test|start|stop|ssh|copy|status]"
    echo ""
    echo "  test    Build and test the Hyperion module inside the VM"
    echo "  start   Start the VM in background"
    echo "  stop    Stop the running VM"
    echo "  ssh     Open an interactive SSH session into the VM"
    echo "  copy    Copy Hyperion source into the VM (no build/test)"
    echo "  status  Check if VM is running"
    exit 1
}

start_vm() {
    if vm_is_running; then
        log_info "VM is already running"
        return 0
    fi

    if [[ ! -f "$DISK_IMG" ]]; then
        die "Disk image not found. Run setup first: bash scripts/setup_vm.sh"
    fi

    if [[ ! -f "$SSH_KEY" ]]; then
        die "SSH key not found. Run setup first: bash scripts/setup_vm.sh"
    fi

    log_info "Starting VM in background..."

    qemu-system-x86_64 \
        -enable-kvm \
        -cpu host,+vmx \
        -m 4096 \
        -smp 2 \
        -drive file="$DISK_IMG",format=qcow2,if=virtio \
        -nic user,model=virtio,hostfwd=tcp::${SSH_PORT}-:22 \
        -nographic \
        -display none \
        -pidfile "$VM_DIR/vm.pid" \
        > "$VM_LOG" 2>&1 &

    wait_for_ssh
    log_ok "VM is running (SSH port ${SSH_PORT})"
}

stop_vm() {
    if ! vm_is_running; then
        log_info "VM is not running"
        return 0
    fi

    log_info "Stopping VM..."

    ssh_cmd "sudo shutdown -h now" 2>/dev/null || true

    for i in {1..15}; do
        if ! vm_is_running; then
            log_ok "VM shut down gracefully"
            rm -f "$VM_DIR/vm.pid"
            return 0
        fi
        sleep 2
    done

    log_warn "VM did not shut down, force-killing..."
    pkill -f "qemu-system.*hyperion-vm" 2>/dev/null || true
    sleep 1
    rm -f "$VM_DIR/vm.pid"
    log_ok "VM force-stopped"
}

copy_sources() {
    log_info "Copying Hyperion source to VM..."
    ssh_cmd "sudo mkdir -p /opt/hyperion && sudo chown ubuntu:ubuntu /opt/hyperion"

    rsync -az \
        -e "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -i $SSH_KEY -p $SSH_PORT" \
        --exclude='*.o' --exclude='*.ko' --exclude='*.mod*' \
        --exclude='.cache' --exclude='modules.order' --exclude='Module.symvers' \
        --exclude='vm/' --exclude='scripts/' --exclude='docs/' \
        --exclude='.git' --exclude='*.png' \
        "$PROJECT_DIR/" "ubuntu@localhost:/opt/hyperion/" 2>&1 | tail -3

    log_ok "Source copied to VM:/opt/hyperion"
}

run_test() {
    echo ""
    echo "=============================================="
    echo " Hyperion VM Test Suite"
    echo "=============================================="
    echo ""

    start_vm
    copy_sources

    log_info "Building Hyperion inside VM..."
    ssh_cmd 'bash -s' << 'BUILD' || { log_err "Build failed"; stop_vm; exit 1; }
set -e
echo "=== Building kernel module ==="
cd /opt/hyperion/module && make clean 2>&1 | tail -1 && make 2>&1
echo "=== Building user program ==="
cd /opt/hyperion/user && make clean 2>&1 | tail -1 && make 2>&1
echo "=== Build complete ==="
BUILD
    log_ok "Build succeeded"

    echo ""
    log_info "Running Hyperion test..."
    echo ""

    local test_rc=0
    ssh_cmd 'bash -s' << 'TEST' || test_rc=$?
set -euo pipefail

PASS=0; FAIL=1
START_DMESG="$(sudo dmesg | wc -l)"

echo "--- TEST: Loading module ---"
sudo dmesg -c > /dev/null 2>&1 || true
if sudo insmod /opt/hyperion/module/hyperion.ko 2>&1; then
    echo "  insmod: OK"
else
    echo "  insmod: FAILED"
    sudo dmesg | tail -40
    exit $FAIL
fi

if ! lsmod | grep -q hyperion; then
    echo "  lsmod check: FAILED (module not loaded)"
    exit $FAIL
fi
echo "  lsmod check: OK"

if [[ -e /dev/hyperion ]]; then
    echo "  device node: OK (/dev/hyperion exists)"
else
    echo "  device node: FAILED (/dev/hyperion missing)"
    exit $FAIL
fi

echo ""
echo "--- TEST: IOCTL_INIT_VMX ---"
echo "" | sudo timeout 20 /opt/hyperion/user/hyperion-user 2>&1 || true

sleep 2

echo ""
echo "--- KERNEL LOG (dmesg) ---"
sudo dmesg
echo "--- END KERNEL LOG ---"

echo ""
echo "--- ANALYSIS ---"
KLOG=$(sudo dmesg)

PANIC=0; VMLAUNCH_ERR=0; VMX_INIT=0; VM_EXITS=0

if echo "$KLOG" | grep -qiE "(Kernel panic|BUG:|Oops:|general protection fault|RIP:.*hyperion|unable to handle)"; then
    echo "  [CRITICAL] Kernel panic/OOPS detected!"
    PANIC=1
fi

if echo "$KLOG" | grep -q "VMLAUNCH error"; then
    VMLAUNCH_ERR=1
    VMLAUNCH_CODE=$(echo "$KLOG" | grep "VMLAUNCH error" | head -1)
    echo "  [ERROR] VMLAUNCH_FAILED: $VMLAUNCH_CODE"
fi

if echo "$KLOG" | grep -q "VMLAUNCH"; then
    if echo "$KLOG" | grep -q "VMLAUNCH on CPU"; then
        echo "  [OK] VMLAUNCH executed (no error logged)"
    fi
fi

if echo "$KLOG" | grep -q "VMX initialized successfully"; then
    VMX_INIT=1
    echo "  [OK] VMX_INIT_OK: VMX initialization completed"
fi

if echo "$KLOG" | grep -q "VMX initialization failed"; then
    echo "  [FAIL] VMX_INIT_FAILED"
fi

if echo "$KLOG" | grep -q "VM_EXIT_REASON"; then
    VM_EXITS=1
    COUNT=$(echo "$KLOG" | grep -c "VM_EXIT_REASON")
    echo "  [OK] VM_EXITS_DETECTED: $COUNT VM-exit(s) handled"
fi

if echo "$KLOG" | grep -q "VMX is not supported"; then
    echo "  [FAIL] VMX_NOT_SUPPORTED in nested VM (check -cpu host,+vmx flag)"
    PANIC=1
fi

echo ""
echo "--- CLEANUP ---"
if sudo rmmod hyperion 2>&1; then
    echo "  rmmod: OK"
else
    echo "  rmmod: FAILED (VM may need reboot)"
fi

echo ""
if [[ $PANIC -eq 1 ]]; then
    echo "=== OVERALL: FAIL (kernel panic/crash) ==="
    exit $FAIL
elif [[ $VMLAUNCH_ERR -eq 1 ]]; then
    echo "=== OVERALL: VMLAUNCH_FAILED (expected — guest IA32_EFER missing) ==="
    exit $FAIL
elif [[ $VMX_INIT -eq 1 ]]; then
    echo "=== OVERALL: PASS ==="
    exit $PASS
else
    echo "=== OVERALL: UNKNOWN (check dmesg output) ==="
    exit $FAIL
fi
TEST

    echo ""
    if [[ $test_rc -eq 0 ]]; then
        log_ok "Test PASSED"
    else
        log_warn "Test completed with issues (see analysis above)"
        log_info "The VMLAUNCH failure is expected — you need to set GUEST_IA32_EFER in the VMCS."
        log_info "Use '$0 ssh' to debug interactively inside the VM."
    fi

    echo ""
    log_info "VM is still running. Commands:"
    echo "  ${CYAN}$0 ssh${NC}     - interactive shell in VM"
    echo "  ${CYAN}$0 stop${NC}    - shut down VM"
}

# ---- Dispatch ----

case "${MODE:-}" in
    test)    run_test ;;
    start)   start_vm; log_info "VM running. Use '$0 ssh' to connect, '$0 stop' to stop." ;;
    stop)    stop_vm ;;
    ssh)     start_vm; log_info "Connecting (Ctrl+D to exit)..."; ssh_cmd ;;
    copy)    start_vm; copy_sources ;;
    status)  vm_is_running && log_ok "VM is running" || log_info "VM is not running" ;;
    *)       usage ;;
esac
