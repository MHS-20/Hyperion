# Hyperion

Hyperion is a Linux kernel module implementing a **Type-2 hypervisor** using Intel VT-x with Extended Page Tables (EPT). It self-virtualizes the running kernel — meaning the same kernel that loads the module becomes the guest — and exposes control interfaces through a character device for userspace tooling.

<div align="center">
<img src="hyperion.png" alt="Logo" width="300"/>
</div>
---

## Architecture

Hyperion consists of two components that share a dual-role header:

| Component | Location | Role |
|---|---|---|
| **Kernel module** (`hyperion.ko`) | `module/` | VT-x initialization, VMXON/VMCS management, EPT identity mapping, VM-exit handling, page hooking, event injection, VPID TLB management, message passing |
| **Userspace control program** | `user/` + `test/` | Opens `/dev/hyperion`, sends IOCTLs to trigger virtualization and run feature tests |

The shared header switches behavior via `#ifdef __KERNEL__`, exposing kernel-side APIs (allocations, VMREAD/VMWRITE wrappers, per-CPU state) to the module and IOCTL/VMCALL constants to userspace.

### Module-internal component map

- **Char device driver** — module init/exit, `/dev/hyperion` creation, IOCTL dispatch, CR4.VMXE (enable VMX operation)
- **VMX core** — CPU capability detection, per-CPU VMXON/VMCS lifecycle, VMCS field configuration, VMLAUNCH/VMRESUME, VM-exit handler (C-level dispatch), VMCALL service handler, syscall hook demonstrator
- **EPT / memory** — MTRR-to-EPT memory-type mapping, 4-level identity page table builder, 2 MB-to-4 KB page splitting, hidden hook infrastructure (read/write/execute), INVEPT/INVVPID cache invalidation, page-fault-style EPT-violation handler
- **Assembly trampoline** — GPR save/restore on VM-exit, VMXOFF state restoration, VMCALL entry point
- **Log buffer** — lock-free circular buffer for VMX-root-mode to userspace message passing

---

## Features

### VT-x Virtualization (Type-2)

The module enters VMX root operation by setting CR4.VMXE, allocating VMXON and VMCS regions, and executing VMXON on every logical CPU. A single VMCS per CPU is prepared by capturing the live host state — GDT, IDT, segment registers, control registers, MSRs, and descriptor-table bases — and loading them as the guest state. The host-state area in the VMCS is configured to vector all VM-exits to the module's own handler.

After VMLAUNCH, the CPU transitions to VMX non-root mode and continues executing the very kernel code that launched it. The hypervisor sees every VM-exit and can inspect, modify, or reject the guest's behavior before resuming.

### Extended Page Tables (EPT)

EPT provides a second level of address translation: the guest's physical addresses are further translated through an EPT hierarchy before reaching the true host-physical memory. The module builds a 4-level identity-mapped EPT (PML4 → PDPT → PD → PT) covering the entire 512 GB physical address space.

- Initial setup uses 2 MB large pages for performance.
- Memory-type attributes (Write-Back, Uncacheable, etc.) are derived from the platform's MTRR variable ranges. When no MTRR ranges are detected, known MMIO regions (VGA hole, APIC space, PCI MMIO above 4 GB) are forced to Uncacheable to prevent hangs.
- The EPT pointer is installed in the VMCS via the EPTP field, and the secondary processor-based control `ENABLE_EPT` is set.

### Hidden Hooks (EPT-Based Page Monitoring)

Because all guest-physical memory accesses pass through EPT, the hypervisor can silently intercept reads, writes, or instruction fetches on any 4 KB page without the guest's knowledge. This is fundamentally undetectable from within the guest — the guest's own page tables are untouched.

#### Read/Write Hooks

When a read or write hook is installed on a page:

1. The 2 MB EPT entry is split into 512 individual 4 KB PTEs.
2. The target page's PTE has read or write permission cleared.
3. Any access by the guest triggers an EPT Violation VM-exit.
4. The handler logs the access (guest RIP, type, physical address), temporarily restores the original permissions, sets the Monitor Trap Flag, and resumes the guest.
5. After one instruction executes, the Monitor Trap Flag fires a VM-exit. The handler re-applies the restricted permissions and clears MTF.

#### Execute Hooks (Function Redirection)

Execute hooks redirect execution from a target kernel function to a hook function:

1. A "fake page" is allocated and the original page's contents are copied in.
2. At the function's offset within the fake page, an absolute 64-bit jump trampoline is written (`mov r15, target; push r15; ret`).
3. The EPT entry for the target physical page is swapped to point to the fake page, with read and write disabled but execute enabled.
4. When the guest executes the hooked function, it runs the trampoline and lands in the hook callback.
5. When the guest reads or writes the page (e.g., attempting to inspect the hook), an EPT Violation fires. The handler temporarily swaps back to the real page, allowing the access, then MTF re-hides the fake page.

All active hooks are tracked in a linked list. Hooks can be dynamically installed and uninstalled from VMX root mode via the VMCALL interface.

### Syscall Hooking

The Linux syscall table holds function pointers for every system call. By locating `sys_call_table` and applying an EPT execute hook on a specific syscall handler, the hypervisor intercepts every invocation of that syscall system-wide.

This is more powerful than conventional syscall hooking:

- The syscall table itself is never modified — kernel integrity checks see no tampering.
- The interception happens at the physical memory layer, below the OS.
- Arguments arrive in registers following the System V AMD64 ABI and can be inspected, logged, or modified before calling the original handler.

A built-in demonstrator hooks `__x64_sys_openat` and logs every file opened on the system.

### Event Injection & Exception Interception

The VMCS **exception bitmap** controls which exception vectors cause VM-exits instead of being delivered to the guest. By setting the breakpoint bit (vector 3), every `int3` in the guest triggers a VM-exit.

The handler reads the exit interruption information, determines the vector and type, and can:

- **Re-inject** the original event so the guest handles it normally.
- **Inject a different event** — for example, replacing a page fault with a general protection fault.
- **Swallow** the event entirely, hiding it from the guest.

Event injection is done through the **VM-entry interruption-information field**. The module builds the 32-bit field encoding the vector, interruption type (external interrupt, NMI, hardware exception, software interrupt, software exception), an optional error code, and the valid bit. For software exceptions, the VM-entry instruction length is set so the guest's RIP advances correctly.

### Virtual Processor Identifier (VPID)

VPID tags TLB entries with a 16-bit identifier so that VM-entries and VM-exits do not flush the entire TLB. Without VPID, every transition between root and non-root mode would invalidate all cached linear-to-physical translations, devastating performance.

With VPID enabled in the secondary processor-based controls and a non-zero VPID value set in the VMCS:

- TLB entries are tagged with the guest's VPID.
- On VM-exit, only entries tagged with that VPID are eligible for eviction.
- On CR3 changes, `INVVPID` with `SINGLE_CONTEXT` flushes only the current guest's linear mappings.
- `INVVPID` with `ALL_CONTEXT` invalidates TLB entries for all VPIDs except VPID 0 (the host).

The module supports all four INVVPID types: individual address, single context, all contexts, and single context retaining globals.

### Monitor Trap Flag (MTF)

The Monitor Trap Flag is a VM-execution control that causes a VM-exit after every instruction executes. Unlike the architectural Trap Flag (RFLAGS.TF), MTF is invisible to the guest — the guest cannot read or modify it.

MTF is used internally by the hidden hook system to single-step one instruction after temporarily restoring page permissions, then re-apply the hook before the guest continues.

### Custom Message Passing (VMX Root-Mode ↔ Userspace)

Communication between VMX root mode and userspace is challenging because VMX root mode runs with interrupts disabled and cannot safely call most kernel APIs.

Hyperion solves this with a **lock-free circular log buffer**:

- **VMX root mode** writes structured log entries (operation code + message body) using a custom spinlock safe for non-preemptible contexts.
- **Userspace** reads entries via an IOCTL that copies from the kernel buffer to user memory.
- A memory barrier ensures the message header is visible atomically.
- The buffer holds up to 1000 packets of 256 bytes each, acting as a ring.

Reverse-direction communication (userspace → VMX root mode) uses the VMCALL mechanism: an IOCTL stores parameters and executes VMCALL, which causes a VM-exit. The VMCALL handler in VMX root mode reads the parameters from the saved guest registers and dispatches to the requested service.

---

## How It Virtualizes

### Initialization Flow

1. Userspace opens `/dev/hyperion` and sends `IOCTL_INIT_VMX`.
2. The driver calls `enable_vmx_operation()`, which reads the `IA32_VMX_CR0_FIXED0/1` and `IA32_VMX_CR4_FIXED0/1` MSRs and sets the required CR0 and CR4 bits, including CR4.VMXE.
3. CPU capability is verified via CPUID leaf 1 (ECX bit 5 for VMX) and the `IA32_FEATURE_CONTROL` MSR (lock + VMXON-enable bits).
4. The number of online CPUs is queried, and a per-CPU array of guest state structures is allocated.
5. EPT is initialized: MTRR variable ranges are scanned, an identity page table is built, and the EPTP value is computed.
6. The circular log buffer is allocated and zeroed.
7. On each CPU (via `on_each_cpu`):
   - A VMXON region and VMCS region are allocated (page-aligned physical memory).
   - A dedicated VMM stack and MSR bitmap are allocated.
   - The VMXON region's revision identifier is set from `IA32_VMX_BASIC`.
   - VMXON is executed.
   - The VMCS is loaded via VMPTRLD.
8. VMX non-root mode is launched on all CPUs (remote first, then local).

### VMCS Configuration

The VMCS is configured to create a "clone" of the current system:

- **Guest state** mirrors live host state: CR0, CR3, CR4, DR7, RFLAGS, segment selectors with their descriptor-derived bases/limits/access-rights, GDTR/IDTR bases and limits, FS/GS bases, SYSENTER MSRs, IA32_PAT, IA32_EFER.
- **Host state** stores the kernel's CR0, CR3, CR4, FS/GS bases, TR base, GDTR/IDTR bases, and critically: HOST_RIP points to the assembly VM-exit trampoline, HOST_RSP points to the per-CPU VMM stack.
- **Control fields** enable MSR-bitmap filtering, secondary processor-based controls (EPT, VPID, RDTSCP, INVPCID, XSAVE/XRSTORS), IA-32e mode for both entry and exit, and the exception bitmap (breakpoint vector set).
- **The EPTP** is written to the EPT_POINTER field so that all guest-physical accesses are translated through the identity EPT.
- **The VPID** is set to 1.

### VM-Exit Handling

Every VM-exit vectors to the assembly trampoline at HOST_RIP, which:

1. Pushes all 16 general-purpose registers onto the VMM stack.
2. Passes a pointer to the saved register area to the C handler.
3. On return: if the handler signals termination, branches to the VMXOFF path; otherwise restores registers and executes VMRESUME.

The C handler reads `VM_EXIT_REASON` and dispatches:

| Exit Reason | Action |
|---|---|
| CPUID | Emulates CPUID, hides the hypervisor bit (leaf 1, ECX bit 31) and hypervisor leaf 0x40000000 |
| VMCALL | Dispatches to the VMCALL service handler (test, VMXOFF, EPT hooks, INVEPT/INVVPID, logging) |
| CR Access | Updates shadow CRs, syncs guest CR3 on MOV-to-CR3, invalidates VPID-tagged TLB |
| MSR Read/Write | Executes RDMSR/WRMSR for the requested MSR, returns the result in RAX/RDX |
| EPT Violation | Routes to the hidden-hook handler, or falls back to the legacy page-hook exit handler |
| EPT Misconfig | Logs and halts |
| Exception/NMI | Reads interruption info; for breakpoints, logs and re-injects; for others, reflects unchanged |
| Monitor Trap Flag | Re-applies active EPT hook permissions, then clears MTF |
| RDTSC/RDTSCP | Advances RIP past the instruction |
| HLT | Triggers VMXOFF and returns to host |

### Termination Flow

When the device is closed or the module is unloaded:

1. On each CPU, a VMCALL with service `VMCALL_VMXOFF` is dispatched.
2. The VMCALL handler restores the guest's CR3, GDTR, IDTR, FS/GS bases to the physical CPU.
3. It captures the guest's RIP and RSP (advanced past the VMCALL instruction) into per-CPU termination state.
4. It flags that termination has been requested.
5. The assembly handler then executes VMXOFF, pops all saved GPRs, switches to the guest stack, and returns to the instruction after the VMCALL.
6. All per-CPU allocations (VMXON region, VMCS region, VMM stack, MSR bitmap) are freed.
7. The global EPT page table and EPT state are freed.

---

## Page Handling in Detail

### EPT Identity Mapping

The goal is a 1:1 mapping where every guest-physical address translates to the identical host-physical address. This is achieved by building a flat EPT page table:

- **PML4**: A single entry points to the PDPT, covering 512 GB.
- **PDPT**: 512 entries, each pointing to a PD covering 1 GB.
- **PD**: 512 × 512 entries, each a 2 MB large page covering the corresponding 2 MB physical region.

Each 2 MB entry is assigned a memory type (Uncacheable, Write-Combining, Write-Through, Write-Protect, or Write-Back) based on the platform's MTRR variable ranges. When multiple MTRR ranges overlap a 2 MB region, the most restrictive type wins (Uncacheable takes absolute precedence per Intel SDM Section 11.11.4.1).

Since the mapping is identity, any new memory the kernel allocates is already covered — the physical address was in the table from the start.

### 2 MB → 4 KB Splitting

For fine-grained monitoring, 2 MB pages must be split into 512 individual 4 KB pages. The split process:

1. A pre-allocated buffer (from VMX non-root mode, since VMX root mode cannot safely allocate memory) provides space for the new PML1 table.
2. All 512 PML1 entries are initialized with RWX permissions and identity-mapped physical addresses derived from the parent 2 MB entry's PFN.
3. The 2 MB PD entry is replaced with a pointer to the new PML1 table (the field at bit 7 is cleared, and the physical address field points to the PML1 table).
4. The split structure is added to a linked list for later cleanup.
5. After the split, individual 4 KB pages can have their permissions independently modified.

### Cache Invalidation

Any modification to EPT paging structures requires explicit cache invalidation:

- **INVEPT** invalidates EPT-derived translations (guest-physical → host-physical). Used when EPT entry permissions or physical addresses change. Both single-context (one EPTP) and all-contexts variants are supported.
- **INVVPID** invalidates linear translations (guest-virtual → guest-physical) tagged with a specific VPID. Used on CR3 changes to flush only the current guest's TLB.

Invalidation must be broadcast to all logical CPUs because each CPU independently caches translations derived from the same EPT table.

---

## Hardware Interaction

### CPUID Probing

The module queries CPUID leaf 1 to verify VT-x support (ECX bit 5). It then inspects `IA32_FEATURE_CONTROL` (MSR `0x3A`) to confirm the BIOS has locked VMXON support.

### VMX Capability MSRs

All VMCS control fields are adjusted through the capability MSRs:

- `IA32_VMX_PINBASED_CTLS` / `TRUE_PINBASED_CTLS`
- `IA32_VMX_PROCBASED_CTLS` / `TRUE_PROCBASED_CTLS`
- `IA32_VMX_PROCBASED_CTLS2`
- `IA32_VMX_EXIT_CTLS` / `TRUE_EXIT_CTLS`
- `IA32_VMX_ENTRY_CTLS` / `TRUE_ENTRY_CTLS`

Each MSR encodes allowed 1-settings in the upper 32 bits and required 1-settings in the lower 32 bits. The `TRUE` variants (used when `IA32_VMX_BASIC` bit 55 is 1) expose the actual flexible bits, as some default controls appear as must-be-1 in the non-true MSRs but are actually flexible.

### CR0/CR4 Fixed Bits

Before entering VMX operation, CR0 and CR4 must satisfy the constraints in `IA32_VMX_CR0_FIXED0/1` and `IA32_VMX_CR4_FIXED0/1`. Bits set in FIXED0 must be 1; bits clear in FIXED1 must be 0. The module applies these masks, setting CR4.VMXE in the process.

### VMXON / VMCS Regions

The VMXON region and VMCS region are page-aligned, physically contiguous allocations. Their first 4 bytes carry the VMCS revision identifier from `IA32_VMX_BASIC`. VMXON takes the physical address of the VMXON region; VMPTRLD takes the physical address of the VMCS region. VMCLEAR writes back any cached VMCS data to memory before a new VMCS can be loaded.

### MSR Bitmap

The MSR bitmap is a 4 KB structure divided into four 1 KB regions:

- Read bitmap for MSRs `0x00000000–0x00001FFF`
- Read bitmap for MSRs `0xC0000000–0xC0001FFF`
- Write bitmap for MSRs `0x00000000–0x00001FFF`
- Write bitmap for MSRs `0xC0000000–0xC0001FFF`

Setting a bit causes an MSR read or write to that MSR to trigger a VM-exit instead of executing directly. The module zeroes the bitmap (all bits cleared), meaning every MSR access causes a VM-exit and is emulated by the handler.

---

## How to Build and Test

WARNING: testing the hypervisor natively may lead to kernel panics, it is highly recommended to use QEMU/KVM (see below).

### Building

Kernel headers for the running kernel must be installed at `/lib/modules/$(uname -r)/build`.

```bash
# Kernel module
make -C module

# Userspace control program
make -C user

# Test suite
make -C test
```

### Running

```bash
# Load the module
sudo insmod module/hyperion.ko

# Run the test suite
sudo ./test/hyperion-test

# Or run the interactive control program
sudo ./user/hyperion-user

# Unload
sudo rmmod hyperion
```

### Quick smoke test

```bash
sudo ./test.sh
```

### Nested VM Testing (QEMU)

The `scripts/` directory provides a QEMU-based nested-KVM test environment:

```bash
# One-time setup (downloads Ubuntu cloud image, ~650 MB)
bash scripts/setup_vm.sh

# Run the full test suite inside the VM
bash scripts/test_vm.sh

# Interactive SSH into the running VM
bash scripts/test_vm.sh ssh

# Stop the VM
bash scripts/test_vm.sh stop
```

Requirements: `qemu-system-x86_64`, nested KVM (`kvm_intel nested=1`), SSH, rsync, and a cloud-init ISO tool (`cloud-localds`, `genisoimage`, or `mkisofs`).

### Reading Test Output

The test suite verifies features through two channels:

- **Log buffer** (userspace-visible): Circular buffer read via `IOCTL_READ_LOG_BUFFER`. Tests check for expected substrings like `tag=0xaaa`.
- **Kernel ring buffer** (dmesg): Many features log to `printk` because they run in VMX root mode where the log buffer may not be accessible. Check with `dmesg | grep Hyperion`.

Expected dmesg output for working features:

```
Hyperion: module loading
Hyperion: device created at /dev/hyperion
[*] Hyperion: Total MTRR Ranges Committed: N
[*] Hyperion: EPT pointer allocated at 0x...
[*] Hyperion: VMX initialized successfully
[*] Hyperion: VMLAUNCH succeeded on CPU...
[*] Hyperion: VPID test — current VPID = 1
[*] Hyperion: InvvpidSingleContext(1) succeeded
[*] Hyperion: InvvpidAllContexts() succeeded
[*] Hyperion: GDTR/IDTR/FS/GS restored
Hyperion: device closed
```

### Test Suite Coverage

| Test | Feature Verified |
|---|---|
| Log buffer roundtrip | Custom message passing (VMX root ↔ userspace) |
| Hidden EPT read/write hook | EPT page monitoring, hook install/trigger/uninstall |
| Event injection / breakpoint | Exception bitmap, #BP interception, event re-injection |
| Hidden execute hook (kfree) | Execute redirection, fake page swapping |
| Syscall hook (openat) | `sys_call_table` lookup, EPT execute hook on syscall handler |
| VPID-based TLB management | VPID readback, INVVPID single context, INVVPID all contexts |
| Stability | Multi-second CPUID loop under VMX non-root mode |

---

## Module Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `enable_ept` | `bool` | `true` | Enable Extended Page Tables on module load |

Set at load time:

```bash
sudo insmod module/hyperion.ko enable_ept=0   # disable EPT
sudo insmod module/hyperion.ko enable_ept=1   # enable EPT (default)
```

---

## Known Limitations

- **Single VMCS per CPU** — the module does not support multiple concurrent VMs.
- **EPT identity-mapped only** — the hypervisor does not remap guest-physical to different host-physical addresses. This is intentional for self-virtualization but would need modification for hosting separate guest VMs.
- **MTRR variable ranges may be absent** in some nested virtualization environments, causing all memory to default to Write-Back. A fallback marks known MMIO regions as Uncacheable, but exotic MMIO layouts may still cause issues.
- **Kernel-mode `int3` causes oops** — the event injection test triggers `int3` from kernel context, which Linux treats as a fatal condition. This is a limitation of the test approach, not the injection mechanism.
- **Syscall hook requires `kallsyms_lookup_name`** — on kernels ≥ 5.7, this symbol is no longer exported. The syscall hook code is conditionally compiled and will report unavailability on modern kernels.
- **EPT hooks are single-CPU for hook installation** — EPT modifications from VMX root mode on one CPU require broadcasting INVEPT to all other CPUs, which happens via `on_each_cpu` from VMX non-root mode.
- **No `#VE` (Virtualization Exception) support** — EPT violations always cause VM-exits; the suppress-`#VE` bit is not used.
- **No posted interrupts or virtual APIC** — interrupt virtualization is not implemented.

---

## Security Considerations

- The kernel is tainted when the module loads.
- The module operates with full kernel privileges and can access all physical memory.
- The EPT identity mapping means the guest can theoretically access hypervisor data structures in physical memory. A production hypervisor would use separate EPT views to isolate the VMM.
- No code integrity or attestation mechanisms are in place.
