#!/bin/bash
# Start the Hyperion test VM in a way that survives tool timeouts
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VM_DIR="$PROJECT_DIR/vm"

SSH_KEY="$VM_DIR/vm_ed25519"
DISK_IMG="$VM_DIR/hyperion-vm.qcow2"
VM_LOG="$VM_DIR/vm.log"
PID_FILE="$VM_DIR/vm.pid"
SSH_PORT=2222

# Kill any existing VM
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    kill $OLD_PID 2>/dev/null && sleep 2
    kill -9 $OLD_PID 2>/dev/null || true
    rm -f "$PID_FILE"
fi
pkill -f "qemu-system.*hyperion" 2>/dev/null || true
sleep 1

# Start QEMU with nohup so it survives parent death
nohup qemu-system-x86_64 \
    -enable-kvm \
    -cpu host,+vmx \
    -m 4096 \
    -smp 2 \
    -drive file="$DISK_IMG",format=qcow2,if=virtio \
    -nic user,model=virtio,hostfwd=tcp::${SSH_PORT}-:22 \
    -nographic \
    -display none \
    > "$VM_LOG" 2>&1 &

QPID=$!
echo $QPID > "$PID_FILE"
echo "QEMU started with PID $QPID"

# Wait for SSH
for i in $(seq 1 90); do
    if ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
           -o IdentitiesOnly=yes -o ConnectTimeout=2 \
           -i "$SSH_KEY" -p "$SSH_PORT" ubuntu@localhost "echo ok" 2>/dev/null; then
        echo "SSH ready"
        exit 0
    fi
    sleep 2
done

echo "ERROR: SSH did not become available within 180 seconds"
exit 1
