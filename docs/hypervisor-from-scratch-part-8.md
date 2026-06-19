# Hypervisor From Scratch — Part 8: How To Do Magic With Hypervisor!

Welcome to the 8th part of the *Hypervisor From Scratch* tutorial series. If you reached here, you probably finished the 7th part — which was the most challenging part to understand, so hats off to you.

The 8th part is exciting as we'll see real-world examples of solving reverse-engineering related problems with hypervisors. We'll implement hidden hooks, syscall hooks, event injection, VPID-based TLB management, and a custom message-passing mechanism between VMX root mode and user space.

Besides kernel-related concepts, we'll also see CPU-level topics like VPIDs and how Meltdown/Spectre mitigations (KPTI) affect hypervisor design.

---

## Event Injection

One of the essential parts of hypervisors is the ability to inject events (interrupts, exceptions, NMIs, SMIs) as if they arrived normally, and to monitor received interrupts and exceptions.

This gives us great power over the guest operating system. For example, if you're developing a security application, you can intercept breakpoint interrupts and decide whether to pass them through or block them — effectively disabling debuggers from within the hypervisor. You can also inject breakpoints for reverse-engineering purposes, bypassing anti-debugging techniques.

> **What is an "event" to the CPU?**  
> Intel x86 defines two overlapping categories: **vectored events** (interrupts vs exceptions) and **exception classes** (faults vs traps vs aborts). Vectored events cause the processor to jump into an interrupt handler after saving enough state to resume later. Each event has a **vector** (0-255) that indexes into the Interrupt Descriptor Table (IDT), telling the CPU which handler to run.

### Interrupts vs Exceptions

**Interrupts** occur at random times during program execution in response to hardware signals. System hardware uses interrupts to handle events external to the processor, such as I/O completion. Software can also generate interrupts via the `INT n` instruction.

**Exceptions** occur when the processor detects an error condition while executing an instruction (division by zero, page fault, protection violation, etc.). The processor detects a variety of error conditions and raises specific exception vectors.

### Exception Classifications

Exceptions are classified as **faults**, **traps**, or **aborts**:

- **Faults** are exceptions that can be corrected. The saved RIP points to the faulting instruction, so after the handler runs, the instruction is re-executed. Example: page fault (#PF) — the kernel faults in the page then retries the access.

- **Traps** are reported immediately AFTER the trapping instruction executes. The saved RIP points to the next instruction. Examples: breakpoint (#BP) after `int3`, or single-step (#DB) after each instruction when TF is set.

- **Aborts** don't reliably report the instruction boundary. They're used for hardware errors (machine check, double fault) and typically can't be recovered from.

> **Why does this matter for a hypervisor?**  
> When you inject an event, you need to classify it correctly. If you inject a fault-type exception but set the VM-entry instruction length to 0, the guest will re-execute the instruction, potentially causing an infinite loop. If you inject a trap-type exception without setting the instruction length, the guest will skip the correct next instruction.

### Event Injection Fields

Event injection is done using the **VM-entry interruption-information field** of the VMCS. This 32-bit field is written before VMRESUME; after all guest context has been loaded (MSRs, registers), the CPU delivers the event through the IDT using the specified vector.

Three VMCS fields control event injection:
- `VM_ENTRY_INTR_INFO_FIELD` (32 bits) — details about the event to inject
- `VM_ENTRY_EXCEPTION_ERROR_CODE` (32 bits) — error code for exceptions that push one
- `VM_ENTRY_INSTRUCTION_LEN` (32 bits) — length of the trapping instruction (for software interrupts/exceptions)

The VM-entry interruption-information field layout:

| Bits   | Field                    |
|--------|--------------------------|
| 7:0    | Vector (0-255)           |
| 10:8   | Interruption type        |
| 11     | Deliver error code (1=yes) |
| 12     | Reserved                 |
| 30:13  | Reserved                 |
| 31     | Valid (must be 1)        |

The interruption type values:

```c
/* file: module/vmx.h */
#define INTERRUPT_TYPE_EXTERNAL_INTERRUPT       0
#define INTERRUPT_TYPE_RESERVED                 1
#define INTERRUPT_TYPE_NMI                      2
#define INTERRUPT_TYPE_HARDWARE_EXCEPTION       3
#define INTERRUPT_TYPE_SOFTWARE_INTERRUPT       4
#define INTERRUPT_TYPE_PRIVILEGED_SOFTWARE_INTERRUPT 5
#define INTERRUPT_TYPE_SOFTWARE_EXCEPTION       6
#define INTERRUPT_TYPE_OTHER_EVENT              7
```

Exception vectors (x86 standard):

```c
/* file: module/vmx.h */
#define EXCEPTION_VECTOR_DIVIDE_ERROR       0
#define EXCEPTION_VECTOR_DEBUG              1
#define EXCEPTION_VECTOR_NMI                2
#define EXCEPTION_VECTOR_BREAKPOINT         3
#define EXCEPTION_VECTOR_OVERFLOW           4
#define EXCEPTION_VECTOR_BOUND_RANGE        5
#define EXCEPTION_VECTOR_INVALID_OPCODE     6
#define EXCEPTION_VECTOR_DEVICE_NOT_AVAILABLE 7
#define EXCEPTION_VECTOR_DOUBLE_FAULT       8
#define EXCEPTION_VECTOR_COPROCESSOR_SEGMENT_OVERRUN 9
#define EXCEPTION_VECTOR_INVALID_TSS        10
#define EXCEPTION_VECTOR_SEGMENT_NOT_PRESENT 11
#define EXCEPTION_VECTOR_STACK_FAULT        12
#define EXCEPTION_VECTOR_GENERAL_PROTECTION 13
#define EXCEPTION_VECTOR_PAGE_FAULT         14
#define EXCEPTION_VECTOR_RESERVED_15        15
#define EXCEPTION_VECTOR_FLOATING_POINT     16
#define EXCEPTION_VECTOR_ALIGNMENT_CHECK    17
#define EXCEPTION_VECTOR_MACHINE_CHECK      18
#define EXCEPTION_VECTOR_SIMD_FLOATING_POINT 19
#define EXCEPTION_VECTOR_VIRTUALIZATION     20
#define EXCEPTION_VECTOR_CONTROL_PROTECTION 21
```

### Error Codes

Some exceptions push a 32-bit error code onto the stack. The following table shows which exceptions do and don't:

| Vector | Exception           | Error Code? |
|--------|---------------------|-------------|
| 0      | #DE (Divide Error)  | No          |
| 1      | #DB (Debug)         | No          |
| 3      | #BP (Breakpoint)    | No          |
| 8      | #DF (Double Fault)  | Yes (always 0) |
| 10     | #TS (Invalid TSS)   | Yes         |
| 11     | #NP (Segment Not Present) | Yes    |
| 12     | #SS (Stack Fault)   | Yes         |
| 13     | #GP (General Protection) | Yes     |
| 14     | #PF (Page Fault)    | Yes         |
| 17     | #AC (Alignment Check) | Yes       |
| 21     | #CP (Control Protection) | Yes     |

All other exception vectors do NOT push an error code. When injecting an exception that does not push an error code, the `deliver-error-code` bit in the interruption-information field should be 0.

### Vectored Event Injection

The function to inject an event into the guest:

```c
/* file: module/vmx.c */
static void EventInjectInterruption(uint32_t InterruptionType,
                                     uint32_t Vector,
                                     bool DeliverErrorCode,
                                     uint32_t ErrorCode) {
  uint32_t IntrInfo = 0;

  /* Build the interruption-information field */
  IntrInfo |= Vector;                          /* bits 7:0   */
  IntrInfo |= (InterruptionType & 0x7) << 8;   /* bits 10:8  */
  IntrInfo |= (DeliverErrorCode ? 1 : 0) << 11; /* bit 11    */
  IntrInfo |= (1 << 31);                       /* bit 31: valid */

  vmwrite(VM_ENTRY_INTR_INFO_FIELD, IntrInfo);

  if (DeliverErrorCode)
    vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, ErrorCode);
}
```

Injecting a breakpoint (#BP, INT3):

```c
/* file: module/vmx.c */
static void EventInjectBreakpoint(uint64_t GuestRip) {
  uint64_t ExitInstructionLength = 0;

  vmread(VM_EXIT_INSTRUCTION_LEN, &ExitInstructionLength);

  EventInjectInterruption(INTERRUPT_TYPE_SOFTWARE_EXCEPTION,
                          EXCEPTION_VECTOR_BREAKPOINT,
                          false, /* no error code for #BP */
                          0);

  /*
   * For software exceptions (INT3, INTO, BOUND), the VM-entry
   * instruction length must be set so the guest knows how far to
   * advance RIP.  Without this, the guest may infinite-loop on
   * the INT3 instruction.
   */
  vmwrite(VM_ENTRY_INSTRUCTION_LEN, ExitInstructionLength);
}
```

> **Trap vs Fault injection:**  
> For trap-type events (like #BP), the CPU expects the saved RIP to point PAST the trapping instruction. The `VM_ENTRY_INSTRUCTION_LEN` tells the CPU how many bytes to skip. For fault-type events (like #GP, #PF), you should NOT set the instruction length — the CPU re-executes the faulting instruction. This matches the x86 exception model.

Injecting a general protection fault (#GP with error code 0):

```c
/* file: module/vmx.c */
static void EventInjectGeneralProtection(uint64_t ErrorCode) {
  EventInjectInterruption(INTERRUPT_TYPE_HARDWARE_EXCEPTION,
                          EXCEPTION_VECTOR_GENERAL_PROTECTION,
                          true, /* #GP pushes an error code */
                          ErrorCode);
}
```

---

## Exception Bitmap

The **exception bitmap** is a 32-bit VM-execution control field in the VMCS. When bit N is set, exception vector N causes a VM-exit with reason `EXIT_REASON_EXCEPTION_NMI` (0) instead of being delivered normally to the guest.

This is how we intercept specific exceptions for monitoring or modification. For example, to intercept breakpoints (#BP, vector 3), set bit 3:

```c
/* file: module/vmx.c — inside setup_vmcs() or before VMLAUNCH */
uint64_t exception_bitmap = (1 << EXCEPTION_VECTOR_BREAKPOINT); /* bit 3 */
vmwrite(EXCEPTION_BITMAP, exception_bitmap);
```

> **Only 32 exception vectors exist** (0-31). Vectors 32-255 are external interrupts and are intercepted using the pin-based "external-interrupt exiting" control, not the exception bitmap.

When an intercepted exception fires, the VM-exit handler reads `VM_EXIT_INTR_INFO` and `VM_EXIT_INTR_ERROR_CODE`:

```c
/* file: module/vmx.c — inside main_vmexit_handler() */
case EXIT_REASON_EXCEPTION_NMI: {
    uint64_t IntrInfo = 0;
    uint64_t IntrErrorCode = 0;

    vmread(VM_EXIT_INTR_INFO, &IntrInfo);
    uint32_t Vector = IntrInfo & 0xFF;
    uint32_t InterruptionType = (IntrInfo >> 8) & 0x7;

    if (Vector == EXCEPTION_VECTOR_BREAKPOINT &&
        InterruptionType == INTERRUPT_TYPE_SOFTWARE_EXCEPTION) {
        uint64_t GuestRip = 0;
        vmread(GUEST_RIP, &GuestRip);
        printk(KERN_INFO "[*] Hyperion: #BP at RIP=0x%llx on CPU %d\n",
               GuestRip, smp_processor_id());

        /*
         * Don't advance RIP — we want to re-inject the breakpoint
         * so the guest can handle it normally (or we could swallow it).
         */
        g_guest_state[smp_processor_id()].increment_rip = false;
        EventInjectBreakpoint(GuestRip);
    } else {
        /* Re-inject the original exception */
        vmwrite(VM_ENTRY_INTR_INFO_FIELD, IntrInfo);
        vmread(VM_EXIT_INTR_ERROR_CODE, &IntrErrorCode);
        vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, IntrErrorCode);
    }
    break;
}
```

---

## Monitor Trap Flag (MTF)

The **Monitor Trap Flag** is a VM-execution control that causes a VM-exit with reason `EXIT_REASON_MONITOR_TRAP_FLAG` (37) after every single instruction executes. Unlike the regular Trap Flag (RFLAGS.TF), MTF is invisible to the guest — the guest cannot see or modify it.

> **Why MTF matters for hidden hooks:**  
> When we use EPT to trap on memory access (e.g., unsetting read permissions), the EPT violation handler needs to temporarily restore the original permissions so the trapped instruction can execute. But we need to re-apply the hook after that ONE instruction. MTF gives us exactly that: after the single instruction executes, we get a VM-exit where we can re-apply the hook permissions. This is how we achieve single-step visibility at the hypervisor level.

```c
/* file: module/vmx.c */
static void HvSetMonitorTrapFlag(bool Enable) {
    uint64_t CpuBasedVmExecControls = 0;

    vmread(CPU_BASED_VM_EXEC_CONTROL, &CpuBasedVmExecControls);

    if (Enable)
        CpuBasedVmExecControls |= CPU_BASED_MONITOR_TRAP_FLAG;
    else
        CpuBasedVmExecControls &= ~CPU_BASED_MONITOR_TRAP_FLAG;

    vmwrite(CPU_BASED_VM_EXEC_CONTROL, CpuBasedVmExecControls);
}
```

The MTF VM-exit handler:

```c
/* file: module/vmx.c — inside main_vmexit_handler() */
case EXIT_REASON_MONITOR_TRAP_FLAG: {
    /*
     * After the trapped instruction executed (MTF fires),
     * re-apply the EPT hook permissions and turn off MTF.
     */
    EptHandleMonitorTrapFlag();
    HvSetMonitorTrapFlag(false);

    /* Don't advance RIP — the instruction already executed */
    g_guest_state[smp_processor_id()].increment_rip = false;
    break;
}
```

---

## Hidden Hooks (Simulating Hardware Debug Registers Without Any Limitation)

This is one of the most powerful features of EPT-based hypervisors. Hardware debug registers (DR0-DR3) can only monitor 4 addresses at a time, and they're easily detectable by the guest. With EPT, we can monitor an **unlimited** number of memory locations without the guest being able to detect it.

### Hidden Hook Scenarios

We implement two types of hidden hooks:

1. **Read/Write hooks** — Monitor memory reads or writes to a specific physical page. When the guest accesses the page, an EPT violation occurs, we log the access, temporarily restore the original permissions, set MTF to single-step, then re-apply the hook.

2. **Execute hooks** — Redirect execution from one function to another. We create a "fake page" (Page B) containing a jump to our hook function, then swap the EPT entry to point to Page B instead of the original page (Page A). When the guest tries to execute code on Page A, it actually executes Page B which jumps to our hook.

### Read/Write Hidden Hook Implementation

The flow for a read/write hook:
1. Install hook: clear read and/or write permissions on the EPT entry for the target page
2. Guest reads/writes the page → EPT Violation VM-exit
3. In the handler: log guest RIP, what kind of access (read/write), and the value
4. Restore original EPT entry (re-enable permissions)
5. Set MTF to fire after the next instruction
6. VMRESUME → instruction executes (now allowed)
7. MTF VM-exit fires → re-apply hook permissions (clear read/write again)
8. Disable MTF, VMRESUME → guest continues normally

### Execute Hidden Hook Implementation

The flow for an execute hook:
1. Create a copy of the target page (Page B) in a new allocation
2. At the offset of the function we want to hook, write an absolute jump to our hook function
3. In Page B's EPT entry: read=0, write=0, execute=1 (execute-only page)
4. Swap the EPT entry: Page A's physical address now points to **Page B**
5. Guest tries to read/modify Page A → EPT violation (read/write disabled)
6. In the handler: swap EPT entry back to point to the **real** Page A (with full permissions)
7. Set MTF
8. VMRESUME → guest reads/writes the real page
9. MTF VM-exit → swap EPT back to Page B (execute-only)
10. VMRESUME → guest continues

When the guest executes the hooked function, it jumps through Page B → our hook function → (optionally) call the original function.

### Writing an x86_64 Absolute Jump

To redirect execution, we write a 64-bit absolute jump into the fake page. The pattern is:

```
mov r15, <target_address>    ; 49 BF <8-byte address>
push r15                     ; 41 57
ret                          ; C3
```

This is 12 bytes total. The `push r15; ret` sequence is equivalent to `jmp r15` which x86_64 doesn't have as a direct absolute jump (it only has relative jumps).

```c
/* file: module/ept.c */
static void EptHookWriteAbsoluteJump(void *Destination, uint64_t TargetAddress) {
    uint8_t *Buffer = (uint8_t *)Destination;

    /* mov r15, TargetAddress (49 BF ...) */
    Buffer[0] = 0x49;
    Buffer[1] = 0xBF;
    memcpy(&Buffer[2], &TargetAddress, sizeof(uint64_t));

    /* push r15 (41 57) */
    Buffer[10] = 0x41;
    Buffer[11] = 0x57;

    /* ret (C3) */
    Buffer[12] = 0xC3;
}
```

### The Hook Installation Function

```c
/* file: module/ept.h — add hook tracking structures */
typedef struct _EPT_HOOKED_PAGE_DETAIL {
    uint64_t PhysicalAddress;       /* physical address of the hooked page    */
    uint64_t FakePagePhysAddress;   /* physical address of the fake page      */
    EPT_PML1_ENTRY OriginalEntry;   /* original EPT entry before hooking      */
    EPT_PML1_ENTRY HookedEntry;     /* the hook entry (with permissions off)  */
    void *HookFunction;             /* address of the hook callback function   */
    void *OrigFunction;             /* address to call original function       */
    bool HookedForRead;             /* hook on read access                     */
    bool HookedForWrite;            /* hook on write access                    */
    bool HookedForExecute;          /* hook on execute access                  */
    struct list_head HookedPagesList; /* linked list node                     */
} EPT_HOOKED_PAGE_DETAIL;
```

```c
/* file: module/ept.c */
static LIST_HEAD(g_HookedPagesList);

bool EptPerformPageHook(void *TargetFunction, void *HookFunction,
                        void **OrigFunction, bool HookRead,
                        bool HookWrite, bool HookExecute) {
    int CurrentCoreIndex = smp_processor_id();
    void *VirtualTarget = (void *)((uintptr_t)TargetFunction & ~(PAGE_SIZE - 1));
    uint64_t PhysicalAddress = virtual_to_physical(VirtualTarget);
    EPT_HOOKED_PAGE_DETAIL *HookDetail;
    EPT_PML1_ENTRY *TargetEntry;
    EPT_PML1_ENTRY OriginalEntry;
    void *FakePage;

    /*
     * Pre-allocate a buffer for the hook detail structure and
     * the fake page from the pool manager.  These allocations
     * are safe to use from VMX root mode.
     */
    HookDetail = kmalloc(sizeof(EPT_HOOKED_PAGE_DETAIL), GFP_KERNEL);
    if (!HookDetail) {
        pr_err("[*] Hyperion: failed to allocate hook detail\n");
        return false;
    }
    memset(HookDetail, 0, sizeof(EPT_HOOKED_PAGE_DETAIL));

    /* Split the 2MB page to 4KB if needed */
    void *PreAllocBuffer = g_guest_state[CurrentCoreIndex].pre_allocated_buffer;
    if (!EptSplitLargePage(g_ept_state->EptPageTable, PreAllocBuffer,
                           PhysicalAddress, CurrentCoreIndex)) {
        pr_err("[*] Hyperion: failed to split page for hook\n");
        kfree(HookDetail);
        return false;
    }

    /* Get the PML1 entry for the target page */
    TargetEntry = EptGetPml1Entry(g_ept_state->EptPageTable, PhysicalAddress);
    if (!TargetEntry) {
        pr_err("[*] Hyperion: failed to get PML1 entry\n");
        kfree(HookDetail);
        return false;
    }

    /* Save the original entry */
    OriginalEntry = *TargetEntry;

    /* For execute hooks: create a fake page with a jump */
    if (HookExecute) {
        FakePage = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (!FakePage) {
            pr_err("[*] Hyperion: failed to allocate fake page\n");
            kfree(HookDetail);
            return false;
        }

        /* Copy original page contents to fake page */
        phys_addr_t OrigPhys = TargetEntry->fields.physical_address * PAGE_SIZE;
        void *OrigVirt = physical_to_virtual(OrigPhys);
        memcpy(FakePage, OrigVirt, PAGE_SIZE);

        /* Write absolute jump to hook function at the right offset */
        uint64_t OffsetInPage = (uint64_t)TargetFunction - (uint64_t)VirtualTarget;
        EptHookWriteAbsoluteJump((uint8_t *)FakePage + OffsetInPage,
                                  (uint64_t)HookFunction);

        /* Create new EPT entry pointing to fake page */
        EPT_PML1_ENTRY FakeEntry = OriginalEntry;
        FakeEntry.fields.physical_address =
            virtual_to_physical(FakePage) / PAGE_SIZE;
        FakeEntry.fields.read = 0;       /* trap reads to detect inspection  */
        FakeEntry.fields.write = 0;      /* trap writes to detect patching   */
        FakeEntry.fields.execute = 1;    /* allow execution from fake page    */

        HookDetail->FakePagePhysAddress = virtual_to_physical(FakePage);

        /*
         * Apply the hook: replace the real EPT entry with the fake one.
         * The guest will now execute from the fake page, which jumps to
         * our hook function.
         */
        EptSetPML1AndInvalidateTLB(TargetEntry, FakeEntry);

        if (OrigFunction) {
            /*
             * Provide a way for the hook function to call the original.
             * The original function's first bytes were overwritten with
             * a jump on the fake page, but the real page still has the
             * original code.  We build a trampoline or simply point to
             * the real function bytes for the caller.
             */
            *OrigFunction = TargetFunction;
        }
    } else {
        /* Read/Write hook: just disable permissions on the real page */
        EPT_PML1_ENTRY HookEntry = OriginalEntry;
        HookEntry.fields.read = HookRead ? 0 : 1;
        HookEntry.fields.write = HookWrite ? 0 : 1;

        HookDetail->HookedEntry = HookEntry;
        EptSetPML1AndInvalidateTLB(TargetEntry, HookEntry);
    }

    /* Fill in hook detail and add to linked list */
    HookDetail->PhysicalAddress = PhysicalAddress;
    HookDetail->OriginalEntry = OriginalEntry;
    HookDetail->HookFunction = HookFunction;
    HookDetail->OrigFunction = TargetFunction;
    HookDetail->HookedForRead = HookRead;
    HookDetail->HookedForWrite = HookWrite;
    HookDetail->HookedForExecute = HookExecute;

    list_add(&HookDetail->HookedPagesList, &g_HookedPagesList);

    printk(KERN_INFO "[*] Hyperion: hook applied at phys=0x%llx "
                     "(R=%d W=%d X=%d)\n",
           PhysicalAddress, HookRead, HookWrite, HookExecute);
    return true;
}
```

### Atomic EPT Entry Modification

> **Critical: EPT entries must be modified atomically.**  
> If you clear bits one at a time (e.g., clear read first, then clear write), the CPU could observe an intermediate state where read=0 but write=1 — an illegal combination that causes EPT misconfiguration. Always write the entire `Flags` field in one instruction:

```c
/* file: module/ept.c */
static void EptSetPML1AndInvalidateTLB(EPT_PML1_ENTRY *Entry,
                                        EPT_PML1_ENTRY NewValue) {
    /*
     * Atomic write of the entire EPT entry.  Writing individual bitfields
     * would create a window where the CPU sees an inconsistent state
     * (e.g., read=0, write=1) which causes EPT misconfiguration.
     */
    Entry->all = NewValue.all;

    /* Invalidate the EPT cache so the change takes effect immediately */
    InveptSingleContext(g_ept_state->EptPointer.all);
}
```

### Handling the Hooked Page VM-Exit

```c
/* file: module/ept.c */
bool EptHandleHookedPage(uint64_t PhysicalAddress, uint64_t GuestRip,
                          bool IsReadViolation, bool IsWriteViolation,
                          bool IsExecuteViolation) {
    EPT_HOOKED_PAGE_DETAIL *HookDetail;
    EPT_PML1_ENTRY *TargetEntry;

    /* Find the matching hook in the linked list */
    list_for_each_entry(HookDetail, &g_HookedPagesList, HookedPagesList) {
        if (HookDetail->PhysicalAddress != PhysicalAddress)
            continue;

        TargetEntry = EptGetPml1Entry(g_ept_state->EptPageTable,
                                       PhysicalAddress);
        if (!TargetEntry)
            continue;

        if (HookDetail->HookedForExecute) {
            /* Execute hook: swap back to real page for reads */
            if (IsReadViolation || IsWriteViolation) {
                /*
                 * Guest is reading/writing the hooked page.  Show the
                 * REAL content (not the fake page with the jump).
                 * The hook function's jump is hidden from the guest.
                 */
                EptSetPML1AndInvalidateTLB(TargetEntry,
                                            HookDetail->OriginalEntry);

                /* Log the access */
                printk(KERN_INFO "[*] Hyperion: hidden hook trigger — "
                                 "RIP=0x%llx phys=0x%llx %s\n",
                       GuestRip, PhysicalAddress,
                       IsReadViolation ? "READ" : "WRITE");

                return true;
            }
            /* Execute violation on an execute-only page — re-enable exec */
            if (IsExecuteViolation) {
                EPT_PML1_ENTRY TempEntry = HookDetail->HookedEntry;
                TempEntry.fields.execute = 1;
                EptSetPML1AndInvalidateTLB(TargetEntry, TempEntry);

                /* Re-do the instruction */
                g_guest_state[smp_processor_id()].increment_rip = false;
                return true;
            }
        } else {
            /* Read/Write hook: restore original, set MTF to re-hook */
            EptSetPML1AndInvalidateTLB(TargetEntry,
                                        HookDetail->OriginalEntry);

            printk(KERN_INFO "[*] Hyperion: hidden hook trigger — "
                             "RIP=0x%llx phys=0x%llx %s%s\n",
                   GuestRip, PhysicalAddress,
                   IsReadViolation ? "READ " : "",
                   IsWriteViolation ? "WRITE" : "");

            /* Set MTF to re-apply the hook after one instruction */
            HvSetMonitorTrapFlag(true);

            /* Don't advance RIP — the instruction hasn't executed yet */
            g_guest_state[smp_processor_id()].increment_rip = false;
            return true;
        }
    }

    return false;
}
```

### MTF Handler for Re-Applying Hooks

```c
/* file: module/ept.c */
void EptHandleMonitorTrapFlag(void) {
    EPT_HOOKED_PAGE_DETAIL *HookDetail;

    /*
     * After the single-stepped instruction executed, re-apply all
     * active hooks by restoring their permission-restricted entries.
     */
    list_for_each_entry(HookDetail, &g_HookedPagesList, HookedPagesList) {
        EPT_PML1_ENTRY *TargetEntry;

        TargetEntry = EptGetPml1Entry(g_ept_state->EptPageTable,
                                       HookDetail->PhysicalAddress);
        if (!TargetEntry)
            continue;

        if (!HookDetail->HookedForExecute) {
            /* Re-apply read/write restrictions */
            EptSetPML1AndInvalidateTLB(TargetEntry,
                                        HookDetail->HookedEntry);
        }
    }
}
```

### Removing Hooks

```c
/* file: module/ept.c */
bool EptPageUnHookSinglePage(void *TargetFunction) {
    void *VirtualTarget = (void *)((uintptr_t)TargetFunction & ~(PAGE_SIZE - 1));
    uint64_t PhysicalAddress = virtual_to_physical(VirtualTarget);
    EPT_HOOKED_PAGE_DETAIL *HookDetail, *Temp;
    bool Found = false;

    list_for_each_entry_safe(HookDetail, Temp, &g_HookedPagesList,
                              HookedPagesList) {
        if (HookDetail->PhysicalAddress == PhysicalAddress) {
            EPT_PML1_ENTRY *TargetEntry;

            TargetEntry = EptGetPml1Entry(g_ept_state->EptPageTable,
                                           PhysicalAddress);
            if (TargetEntry) {
                /* Restore the original EPT entry */
                EptSetPML1AndInvalidateTLB(TargetEntry,
                                            HookDetail->OriginalEntry);
            }

            list_del(&HookDetail->HookedPagesList);

            /* Free the fake page if this was an execute hook */
            if (HookDetail->FakePagePhysAddress) {
                void *FakeVirt = physical_to_virtual(
                    HookDetail->FakePagePhysAddress);
                kfree(FakeVirt);
            }

            kfree(HookDetail);
            Found = true;
            printk(KERN_INFO "[*] Hyperion: hook removed at phys=0x%llx\n",
                   PhysicalAddress);
        }
    }

    return Found;
}
```

---

## System-Call Hook

Hooking system calls from a hypervisor gives us visibility into every syscall made by every process on the system. This is extremely powerful for monitoring, security, and reverse engineering.

> **How syscalls work on Linux x86_64:**  
> The kernel exposes function pointers in a table called `sys_call_table`. Each syscall has a number (e.g., `__NR_open` = 2). The userspace `syscall` instruction transitions to kernel mode, the kernel dispatcher looks up `sys_call_table[nr]`, and calls the function. By replacing the function pointer in this table, we can intercept any syscall.

Our approach: find the syscall table, find the target function's physical address, then apply an EPT hidden execution hook on that function. This is undetectable from within the guest because we intercept at the EPT level, not by modifying kernel memory.

### Finding the Syscall Table on Linux

On older kernels, `sys_call_table` was exported. On modern kernels, it's not exported but can be found via pattern scanning or `/proc/kallsyms` (if `kptr_restrict` is 0).

```c
/* file: module/vmx.c */
#include <linux/kallsyms.h>

static unsigned long *g_sys_call_table = NULL;

static bool SyscallHookFindTable(void) {
    /*
     * On kernels built with CONFIG_KALLSYMS, we can look up the
     * sys_call_table address.  This may require CAP_SYSLOG or
     * setting /proc/sys/kernel/kptr_restrict to 0.
     */
    g_sys_call_table = (unsigned long *)kallsyms_lookup_name("sys_call_table");
    if (!g_sys_call_table) {
        /*
         * Fallback: scan kernel memory for the syscall dispatcher
         * pattern and extract the sys_call_table address from it.
         * The exact pattern depends on the kernel version and build.
         * For many kernels, the pattern near entry_SYSCALL_64 is:
         *   FF 14 C5 XX XX XX XX  (call *sys_call_table(,%rax,8))
         */
        pr_err("[*] Hyperion: sys_call_table not found via kallsyms\n");
        return false;
    }

    printk(KERN_INFO "[*] Hyperion: sys_call_table at 0x%px\n",
           g_sys_call_table);
    return true;
}
```

Getting a function address from the syscall table:

```c
/* file: module/vmx.c */
static void *SyscallHookGetFunctionAddress(unsigned int SyscallNumber) {
    if (!g_sys_call_table || SyscallNumber >= __NR_syscalls) {
        pr_err("[*] Hyperion: invalid syscall number %u\n", SyscallNumber);
        return NULL;
    }

    /*
     * On Linux x86_64, sys_call_table[nr] directly holds the function
     * pointer.  No bit-shifting or decoding needed (unlike Windows SSDT).
     */
    return (void *)g_sys_call_table[SyscallNumber];
}
```

### Applying a Syscall Hook

```c
/* file: module/vmx.c — example usage, call from an IOCTL handler */
static int DemonstrateSyscallHook(void) {
    void *TargetFunction;
    void *OriginalFunction = NULL;
    int SyscallNumber = __NR_openat; /* or __NR_open, __NR_read, etc. */

    if (!SyscallHookFindTable())
        return -1;

    TargetFunction = SyscallHookGetFunctionAddress(SyscallNumber);
    if (!TargetFunction) {
        pr_err("[*] Hyperion: syscall %d not found\n", SyscallNumber);
        return -1;
    }

    /*
     * Apply an EPT hidden execution hook on the syscall function.
     * When any process calls openat(), execution redirects to our
     * hook function (SyscallOpenatHook) which can inspect/modify
     * the arguments, then call the original.
     */
    if (!EptPerformPageHook(TargetFunction, SyscallOpenatHook,
                            &OriginalFunction, false, false, true)) {
        pr_err("[*] Hyperion: failed to apply syscall hook\n");
        return -1;
    }

    printk(KERN_INFO "[*] Hyperion: syscall hook applied on __x64_sys_openat\n");
    return 0;
}
```

A simple syscall hook function: intercept `openat()`, log the filename, then call the original:

```c
/* file: module/vmx.c */
/*
 * Hook function for sys_openat (syscall number 257 on x86_64).
 * On Linux, syscall function signatures use the SYSCALL_DEFINE macros,
 * producing functions like __x64_sys_openat(int dfd, const char *filename,
 * int flags, umode_t mode).
 *
 * Parameters arrive in registers: RDI=dfd, RSI=filename, RDX=flags, R10=mode.
 * We declare the hook with the same signature.
 */
typedef long (*openat_t)(int dfd, const char __user *filename,
                          int flags, umode_t mode);

static openat_t OriginalOpenat = NULL;

static long SyscallOpenatHook(int dfd, const char __user *filename,
                               int flags, umode_t mode) {
    char fname[256] = {0};

    /*
     * Copy the filename from user space.  In VMX root mode, we use
     * the kernel's page tables (HOST_CR3 = kernel CR3), so we can
     * access user-space memory via SMAP-aware copy functions.
     */
    if (filename) {
        if (strncpy_from_user(fname, filename, sizeof(fname) - 1) > 0)
            printk(KERN_INFO "[*] Hyperion: openat(\"%s\", flags=0x%x)\n",
                   fname, flags);
    }

    /* Call the original function */
    if (OriginalOpenat)
        return OriginalOpenat(dfd, filename, flags, mode);

    return -ENOSYS;
}
```

> **Note on Linux syscall hooking:**  
> The `sys_call_table` is read-only on modern kernels (protected by write-protection). EPT hooks bypass this entirely because we intercept at the physical memory level — we don't modify the table itself, we redirect the physical page that contains the target function. This is undetectable by kernel integrity checks.

---

## Virtual Processor ID (VPID) & TLB

**VPID (Virtual Processor Identifier)** is a 16-bit tag attached to TLB entries. Without VPID, the CPU assigns VPID=0 to everything and flushes the entire TLB on every VM-entry and VM-exit. This massive TLB flush is extremely expensive.

With VPID enabled, each VMCS gets its own VPID value, and TLB entries are tagged. VM-entries and VM-exits no longer flush the TLB automatically — they only flush entries for the current VPID, or you must manually decide what to invalidate.

> **Important:** When you enable VPID, the CPU stops automatically flushing TLBs. You MUST manually perform INVVPID (or INVEPT for EPT) whenever you change memory mappings or need to ensure consistency.

Enabling VPID in the VMCS:

```c
/* file: module/vmx.c — inside setup_vmcs() */
/* Enable VPID in secondary controls */
vmwrite(SECONDARY_VM_EXEC_CONTROL,
        adjust_controls(CPU_BASED_CTL2_RDTSCP |
                        CPU_BASED_CTL2_ENABLE_EPT |
                        CPU_BASED_CTL2_ENABLE_INVPCID |
                        CPU_BASED_CTL2_ENABLE_VPID |
                        CPU_BASED_CTL2_ENABLE_XSAVE_XRSTORS,
                        MSR_IA32_VMX_PROCBASED_CTLS2));

/* Set VPID to 1 (0 is reserved for the host) */
vmwrite(VIRTUAL_PROCESSOR_ID, 1);
```

We need the VPID VMCS field encoding:

```c
/* file: module/hyperion.h — add to enum vmcs_fields */
VIRTUAL_PROCESSOR_ID = 0x00000000,
```

> **Wait, the VPID encoding is 0x00000000?**  
> Yes! The VPID field is at encoding 0x00000000 in the VMCS. This is an unusual encoding — most fields start at 0x00004000 or higher. It's a 16-bit field in the VMCS. Don't confuse it with uninitialized memory just because it's at address 0.

### INVVPID — Invalidating TLB Entries Based on VPID

INVVPID is the instruction to invalidate TLB entries tagged with a specific VPID. It has 4 types:

```c
/* file: module/ept.h */
typedef enum _INVVPID_TYPE {
    INVVPID_INDIVIDUAL_ADDRESS             = 0,
    INVVPID_SINGLE_CONTEXT                 = 1,
    INVVPID_ALL_CONTEXT                    = 2,
    INVVPID_SINGLE_CONTEXT_RETAIN_GLOBALS  = 3,
} INVVPID_TYPE;

typedef struct _INVVPID_DESCRIPTOR {
    uint16_t VPID;
    uint16_t Reserved1;
    uint32_t Reserved2;
    uint64_t LinearAddress;
} INVVPID_DESCRIPTOR;
```

The INVVPID assembly function (similar to our INVEPT approach from Part 7):

```c
/* file: module/ept.c */
static uint8_t AsmInvvpid(INVVPID_TYPE Type, INVVPID_DESCRIPTOR *Descriptor) {
    struct {
        uint64_t vpid;
        uint64_t linear_address;
    } operand = {0};
    uint8_t result = 0;

    if (Descriptor) {
        operand.vpid = Descriptor->VPID;
        operand.linear_address = Descriptor->LinearAddress;
    }

    asm volatile(".intel_syntax noprefix\n\t"
                 "invvpid %[type], oword ptr [%[desc]]\n\t"
                 "setna %[result]\n\t"
                 ".att_syntax prefix\n\t"
                 : [result] "=q"(result)
                 : [type] "r"((uint64_t)Type),
                   [desc] "r"(&operand)
                 : "cc", "memory");

    return result;
}

/* Invalidate all TLB entries tagged with a specific VPID */
uint8_t InvvpidSingleContext(uint16_t VPID) {
    INVVPID_DESCRIPTOR Descriptor = {0};
    Descriptor.VPID = VPID;
    return AsmInvvpid(INVVPID_SINGLE_CONTEXT, &Descriptor);
}

/* Invalidate all TLB entries for all VPIDs except VPID 0 */
uint8_t InvvpidAllContexts(void) {
    return AsmInvvpid(INVVPID_ALL_CONTEXT, NULL);
}

/* Invalidate a specific linear address for a specific VPID */
uint8_t InvvpidIndividualAddress(uint16_t VPID, uint64_t LinearAddress) {
    INVVPID_DESCRIPTOR Descriptor = {0};
    Descriptor.VPID = VPID;
    Descriptor.LinearAddress = LinearAddress;
    return AsmInvvpid(INVVPID_INDIVIDUAL_ADDRESS, &Descriptor);
}
```

### When to Use INVVPID vs INVEPT

- **INVEPT** — invalidates EPT-derived translations (guest-physical → host-physical mappings). Use when you change EPT paging structures (split pages, modify permissions, change memory types).

- **INVVPID** — invalidates guest-linear → guest-physical translations (the guest's own TLB). Use when the guest changes CR3 (page table switch) and you want to flush only that VPID's TLB, not the entire system.

- **On CR3 changes:** When our CR access handler processes `MOV to CR3`, we should use `InvvpidSingleContext(VPID)` or `InvvpidIndividualAddress(VPID, address)` instead of a global INVEPT. This is more efficient because we flush only the current guest's linear mappings, not all EPT mappings.

### Updating the CR Access Handler for VPID

```c
/* file: module/vmx.c — inside HandleControlRegisterAccess() */
case TYPE_MOV_TO_CR:
    switch (ControlRegister) {
    case 3:
        /*
         * Guest changed CR3.  Update GUEST_CR3 and invalidate
         * only the guest's VPID-tagged TLB entries.
         * Clear bit 63 (PCID no-invalidate flag) to ensure
         * the TLB is properly flushed.
         */
        vmwrite(GUEST_CR3, (*RegPtr & ~(1ULL << 63)));
        InvvpidSingleContext(1); /* VPID 1: our guest */
        break;
    /* ... other CR handling ... */
    }
    break;
```

> **INVVPID vs INVPCID:**  
> INVPCID is a regular instruction (available in ring 0), while INVVPID is a VMX instruction (available only in VMX root mode). They serve similar purposes but at different privilege levels. As a hypervisor, we use INVVPID because we operate in VMX root mode. The guest kernel uses INVPCID (if available) for its own TLB management.

---

## Fixing Previous Design Issues

### Pool Manager for Pre-Allocated Buffers

In Part 7, we had per-CPU pre-allocated buffers for EPT splitting. This had two problems: no synchronization between CPUs, and buffers were exhausted after one split. We replace this with a global pool manager:

```c
/* file: module/ept.h */
#define POOL_MAX_BUFFERS 10

typedef struct _POOL_MANAGER {
    void *Pool[POOL_MAX_BUFFERS];
    volatile unsigned long PoolLock;  /* custom spinlock */
    int PoolIndex;
    size_t BufferSize;
    bool AutoReplenish;
} POOL_MANAGER;
```

```c
/* file: module/ept.c */
static POOL_MANAGER g_EptSplitPool = {0};

static bool PoolManagerInitialize(POOL_MANAGER *Manager,
                                   size_t BufferSize, int Count) {
    memset(Manager, 0, sizeof(POOL_MANAGER));
    Manager->BufferSize = BufferSize;
    Manager->AutoReplenish = true;

    for (int i = 0; i < Count && i < POOL_MAX_BUFFERS; i++) {
        Manager->Pool[i] = kmalloc(BufferSize, GFP_KERNEL);
        if (!Manager->Pool[i]) {
            pr_err("[*] Hyperion: pool allocation failed\n");
            return false;
        }
        memset(Manager->Pool[i], 0, BufferSize);
        Manager->PoolIndex = i;
    }
    Manager->PoolIndex++; /* point to next free slot */

    return true;
}

static void *PoolManagerRequestPool(POOL_MANAGER *Manager) {
    unsigned long flags;

    /* Simple test-and-set spinlock */
    while (test_and_set_bit(0, &Manager->PoolLock))
        cpu_relax();

    void *Buffer = NULL;

    if (Manager->PoolIndex > 0) {
        Manager->PoolIndex--;
        Buffer = Manager->Pool[Manager->PoolIndex];
        Manager->Pool[Manager->PoolIndex] = NULL;
    }

    clear_bit(0, &Manager->PoolLock);

    /*
     * If we depleted the pool and auto-replenish is on,
     * schedule a delayed allocation to refill it.
     * NOTE: we can't call kmalloc from VMX root mode (might sleep),
     * so replenishment happens from VMX non-root mode or a workqueue.
     */
    if (Manager->PoolIndex == 0 && Manager->AutoReplenish)
        printk(KERN_WARNING "[*] Hyperion: pool depleted, "
                            "replenish needed\n");

    return Buffer;
}
```

### Avoiding Interception of CR3 Accesses

Intercepting CR3 load/store is only necessary when using shadow page tables (not EPT). With EPT enabled, these intercepts are pure overhead. We should clear them from the VMCS controls.

However, some of these controls are "default1" (forced to 1 by the processor). To clear them, we must use the "TRUE" capability MSRs if `IA32_VMX_BASIC[55]` is 1:

```c
/* file: module/vmx.c — inside setup_vmcs() */
static uint32_t HvAdjustControls(uint32_t Ctl, uint32_t Msr) {
    uint64_t VmxBasic;
    uint64_t MsrValue;

    rdmsrl(MSR_IA32_VMX_BASIC, VmxBasic);

    /*
     * If bit 55 of IA32_VMX_BASIC is set, use the "true" capability
     * MSRs which expose the actual flexible bits.  These MSRs have
     * different encodings from the default ones.
     */
    if (VmxBasic & (1ULL << 55)) {
        switch (Msr) {
        case MSR_IA32_VMX_PINBASED_CTLS:
            Msr = MSR_IA32_VMX_TRUE_PINBASED_CTLS;
            break;
        case MSR_IA32_VMX_PROCBASED_CTLS:
            Msr = MSR_IA32_VMX_TRUE_PROCBASED_CTLS;
            break;
        case MSR_IA32_VMX_EXIT_CTLS:
            Msr = MSR_IA32_VMX_TRUE_EXIT_CTLS;
            break;
        case MSR_IA32_VMX_ENTRY_CTLS:
            Msr = MSR_IA32_VMX_TRUE_ENTRY_CTLS;
            break;
        /* MSR_IA32_VMX_PROCBASED_CTLS2 has no TRUE variant */
        default:
            break;
        }
    }

    rdmsrl(Msr, MsrValue);
    Ctl &= (uint32_t)(MsrValue >> 32);        /* allowed 1-settings */
    Ctl |= (uint32_t)(MsrValue & 0xFFFFFFFF); /* must-be-1 bits    */

    return Ctl;
}
```

```c
/* file: module/vmx.c — inside setup_vmcs(), primary proc-based controls */
/*
 * When EPT is enabled, we don't need to intercept CR3 accesses.
 * Use HvAdjustControls() which consults the TRUE capability MSRs
 * to determine if these bits can actually be cleared.
 */
uint32_t CpuControls = CPU_BASED_ACTIVATE_MSR_BITMAP |
                        CPU_BASED_ACTIVATE_SECONDARY_CONTROLS;

vmwrite(CPU_BASED_VM_EXEC_CONTROL,
        HvAdjustControls(CpuControls, MSR_IA32_VMX_PROCBASED_CTLS));
```

### Restoring IDTR, GDTR, GS Base, and FS Base Before VMXOFF

When we terminate the hypervisor with VMXOFF, we must restore certain system registers that the guest might have changed. The kernel (or security mechanisms) may detect if these are not restored to their expected values.

```c
/* file: module/vmx.c */
static void HvRestoreRegisters(void) {
    uint64_t GdtrBase, GdtrLimit;
    uint64_t IdtrBase, IdtrLimit;
    uint64_t FsBase, GsBase;

    /*
     * Read the guest's GDTR/IDTR/FS/GS from the VMCS and
     * restore them to the physical CPU before VMXOFF.
     */
    vmread(GUEST_GDTR_BASE, &GdtrBase);
    vmread(GUEST_GDTR_LIMIT, &GdtrLimit);
    vmread(GUEST_IDTR_BASE, &IdtrBase);
    vmread(GUEST_IDTR_LIMIT, &IdtrLimit);
    vmread(GUEST_FS_BASE, &FsBase);
    vmread(GUEST_GS_BASE, &GsBase);

    /* Restore GDTR */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr = {
        .limit = (uint16_t)GdtrLimit,
        .base = GdtrBase
    };
    asm volatile("lgdt %0" :: "m"(gdtr) : "memory");

    /* Restore IDTR */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {
        .limit = (uint16_t)IdtrLimit,
        .base = IdtrBase
    };
    asm volatile("lidt %0" :: "m"(idtr) : "memory");

    /* Restore FS/GS base */
    wrmsrl(MSR_FS_BASE, FsBase);
    wrmsrl(MSR_GS_BASE, GsBase);

    pr_info("[*] Hyperion: GDTR/IDTR/FS/GS restored\n");
}
```

Call this before VMXOFF in the termination path:

```c
/* file: module/vmx.c — inside VmxVmxoff() */
void VmxVmxoff(void) {
    /* ... existing code to save guest RIP/RSP, read GUEST_CR3 ... */

    /* Restore system registers before turning off VMX */
    HvRestoreRegisters();

    /* Signal that VMXOFF was executed */
    g_guest_state[smp_processor_id()].vmxoff_state.is_vmxoff_executed = true;

    /* Execute VMXOFF */
    asm volatile("vmxoff" ::: "cc");

    /* Clear CR4.VMXE after VMXOFF */
    unsigned long cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~(1UL << 13); /* clear VMXE bit */
    asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}
```

> **Why restore GDTR/IDTR before VMXOFF?**  
> While in VMX non-root mode, the guest runs with its own GDT/IDT which we captured during VMCS setup. But some operations in VMX root mode may have modified the host's GDT/IDT. Before returning control to the guest (after VMXOFF), we restore the guest's descriptor tables to avoid inconsistencies. On Linux this is less critical than on Windows (no PatchGuard), but it's still good practice for clean module unloading.

---

## Designing a VMX Root-Mode Compatible Message Tracing Mechanism

One of the biggest challenges when developing a hypervisor is communication: how do you send debug messages or data from VMX root mode to user space? In VMX root mode, interrupts are disabled (EFLAGS.IF = 0), and you can't safely call most kernel functions.

> **What can and can't you do in VMX root mode?**
> - **CAN:** Access any memory in the kernel's direct mapping (HOST_CR3 = kernel CR3), execute CPU instructions, read/write MSRs, use spinlocks with careful design.
> - **CANNOT:** Call functions that might sleep (kmalloc with GFP_KERNEL), acquire mutexes/semaphores, wait for completions, handle interrupts, or do anything that requires preemption.
> - **UNRELIABLE:** Calling most kernel API functions — they may be paged out, may use locks, may trigger page faults, or may deadlock.

Our solution: a custom lock-free (or spinlock-based) circular buffer that can be written from VMX root mode and read from VMX non-root mode (or user space).

### Message Buffer Design

```
+------------------+
| BUFFER_HEADER    |  <- Operation, Length, Valid flag
+------------------+
| Message Body     |  <- Variable-length data
+------------------+
| BUFFER_HEADER    |
+------------------+
| Message Body     |
+------------------+
       ...
```

```c
/* file: module/vmx.h */
#define LOG_BUFFER_MAX_PACKETS   1000
#define LOG_BUFFER_PACKET_SIZE   256
#define LOG_BUFFER_TOTAL_SIZE    (LOG_BUFFER_MAX_PACKETS * \
                                  (LOG_BUFFER_PACKET_SIZE + sizeof(BUFFER_HEADER)))

typedef struct _BUFFER_HEADER {
    uint32_t OperationCode;      /* message type                   */
    uint32_t BufferLength;       /* length of the message body     */
    volatile uint32_t Valid;     /* 0 = free, 1 = has message      */
    uint32_t Reserved;
} BUFFER_HEADER;

/* Operation codes */
#define OPERATION_LOG_INFO      1
#define OPERATION_LOG_WARNING   2
#define OPERATION_LOG_ERROR     3

typedef struct _LOG_BUFFER_POOL {
    void *BufferStartAddress;           /* start of the circular buffer  */
    void *BufferEndAddress;             /* end of the buffer             */
    volatile int CurrentIndexToWrite;   /* next slot to write            */
    volatile int CurrentIndexToSend;    /* next slot to read (to user)   */
    volatile unsigned long BufferLock; /* custom spinlock for this pool  */
} LOG_BUFFER_POOL;
```

### Initializing the Message Buffer

```c
/* file: module/vmx.c */
static LOG_BUFFER_POOL g_LogPool;

static bool LogInitialize(void) {
    void *Buffer;

    Buffer = kmalloc(LOG_BUFFER_TOTAL_SIZE, GFP_KERNEL);
    if (!Buffer) {
        pr_err("[*] Hyperion: failed to allocate log buffer\n");
        return false;
    }
    memset(Buffer, 0, LOG_BUFFER_TOTAL_SIZE);

    g_LogPool.BufferStartAddress = Buffer;
    g_LogPool.BufferEndAddress = (uint8_t *)Buffer + LOG_BUFFER_TOTAL_SIZE;
    g_LogPool.CurrentIndexToWrite = 0;
    g_LogPool.CurrentIndexToSend = 0;
    g_LogPool.BufferLock = 0;

    pr_info("[*] Hyperion: log buffer initialized (%d packets)\n",
            LOG_BUFFER_MAX_PACKETS);
    return true;
}
```

### Sending a Message (VMX Root-Mode Safe)

```c
/* file: module/vmx.c */
static void LogSendBuffer(uint32_t OperationCode, void *Buffer,
                           uint32_t BufferLength) {
    BUFFER_HEADER *Header;
    void *Destination;
    int Index;

    /* Acquire our custom spinlock (safe in VMX root mode) */
    while (test_and_set_bit(0, &g_LogPool.BufferLock))
        cpu_relax();

    Index = g_LogPool.CurrentIndexToWrite;

    /* Calculate the position of the next header in the circular buffer */
    Destination = (uint8_t *)g_LogPool.BufferStartAddress +
                  Index * (LOG_BUFFER_PACKET_SIZE + sizeof(BUFFER_HEADER));

    Header = (BUFFER_HEADER *)Destination;
    Header->OperationCode = OperationCode;
    Header->BufferLength = BufferLength;

    /* Copy the message body after the header */
    if (Buffer && BufferLength > 0 && BufferLength <= LOG_BUFFER_PACKET_SIZE) {
        memcpy((uint8_t *)Destination + sizeof(BUFFER_HEADER),
               Buffer, BufferLength);
    }

    /* Memory barrier: ensure writes are visible before setting Valid */
    smp_wmb();
    Header->Valid = 1;

    /* Advance write index (circular) */
    g_LogPool.CurrentIndexToWrite =
        (Index + 1) % LOG_BUFFER_MAX_PACKETS;

    /* Release spinlock */
    smp_wmb();
    clear_bit(0, &g_LogPool.BufferLock);
}
```

### Reading Messages (from User-Space IOCTL)

```c
/* file: module/vmx.c */
static int LogReadBuffer(void __user *UserBuffer, uint32_t UserBufferSize,
                          uint32_t *BytesWritten) {
    BUFFER_HEADER *Header;
    void *Source;
    int Index;
    int CopySize = 0;

    *BytesWritten = 0;

    while (test_and_set_bit(0, &g_LogPool.BufferLock))
        cpu_relax();

    Index = g_LogPool.CurrentIndexToSend;

    Source = (uint8_t *)g_LogPool.BufferStartAddress +
             Index * (LOG_BUFFER_PACKET_SIZE + sizeof(BUFFER_HEADER));

    Header = (BUFFER_HEADER *)Source;

    /* Check if there's a valid message to read */
    smp_rmb();
    if (Header->Valid) {
        /* Copy the operation code + body to user space */
        uint32_t TotalSize = sizeof(uint32_t) + Header->BufferLength;
        if (TotalSize <= UserBufferSize) {
            if (copy_to_user(UserBuffer, &Header->OperationCode,
                             sizeof(uint32_t)) == 0 &&
                copy_to_user(UserBuffer + sizeof(uint32_t),
                             (uint8_t *)Source + sizeof(BUFFER_HEADER),
                             Header->BufferLength) == 0) {
                CopySize = TotalSize;
            }
        }

        /* Mark as consumed */
        Header->Valid = 0;
        Header->BufferLength = 0;

        /* Advance read index */
        g_LogPool.CurrentIndexToSend =
            (Index + 1) % LOG_BUFFER_MAX_PACKETS;
    }

    clear_bit(0, &g_LogPool.BufferLock);

    *BytesWritten = CopySize;
    return CopySize > 0 ? 0 : -EAGAIN;
}
```

### Adding a Log IOCTL to the Driver

```c
/* file: module/hyperion.h — add to IOCTL definitions */
#define IOCTL_READ_LOG_BUFFER _IOR(HYPERION_MAGIC, 3, unsigned long)
```

```c
/* file: module/driver.c — inside dev_ioctl() */
case IOCTL_READ_LOG_BUFFER: {
    uint32_t BytesWritten = 0;
    int result = LogReadBuffer((void __user *)arg, PAGE_SIZE, &BytesWritten);
    if (result == 0)
        ret = BytesWritten;  /* return number of bytes read */
    else
        ret = result;        /* return error code */
    break;
}
```

### Sending Log Messages from VMX Root Mode

```c
/* file: module/vmx.c — VMX root-mode safe printk alternative */
static void LogInfo(const char *Format, ...) {
    char Buffer[LOG_BUFFER_PACKET_SIZE];
    va_list Args;
    int Length;

    va_start(Args, Format);
    Length = vsnprintf(Buffer, sizeof(Buffer), Format, Args);
    va_end(Args);

    if (Length > 0 && Length < LOG_BUFFER_PACKET_SIZE)
        LogSendBuffer(OPERATION_LOG_INFO, Buffer, Length + 1);
}
```

### User-Space Log Reader

```c
/* file: user/hyperion-user.c — example log reading loop */
static void read_hypervisor_logs(int fd) {
    uint8_t buffer[4096];
    uint32_t op_code;
    int bytes_read;

    while (1) {
        bytes_read = ioctl(fd, IOCTL_READ_LOG_BUFFER, buffer);
        if (bytes_read <= 0) {
            usleep(200000); /* 200ms — no messages available */
            continue;
        }

        op_code = *(uint32_t *)buffer;
        char *message = (char *)(buffer + sizeof(uint32_t));

        switch (op_code) {
        case OPERATION_LOG_INFO:
            printf("[INFO] %s\n", message);
            break;
        case OPERATION_LOG_WARNING:
            printf("[WARN] %s\n", message);
            break;
        case OPERATION_LOG_ERROR:
            printf("[ERR]  %s\n", message);
            break;
        }
    }
}
```

### Sending Messages from User-Mode to VMX Root Mode (via VMCALL)

For the reverse direction — triggering actions in VMX root mode — we can also send data via IOCTL that stores a buffer to be consumed on the next VM-exit, or we can use VMCALL directly:

```c
/* file: user/hyperion-user.c — send a command to the hypervisor */
/* Execute a VMCALL from user space.  This works because the kernel
 * module runs in VMX non-root mode after VMLAUNCH, and the VMCALL
 * instruction in user space will #UD.  Instead, we use an IOCTL
 * that triggers a VMCALL from kernel context. */
```

For more advanced bidirectional communication, the message pool approach can be extended with a second pool for VMX root → user direction and a notification mechanism (e.g., waking a blocked IOCTL via `wake_up_interruptible()` when messages are available).

---

## Discussion (Key Q&A from the Community)

### Can I use kmalloc in VMX root mode?

No, not reliably. `kmalloc(GFP_KERNEL)` may sleep (waiting for memory reclaim), and sleeping in VMX root mode with interrupts disabled causes a deadlock. `kmalloc(GFP_ATOMIC)` might work but can fail. The solution: pre-allocate buffers from VMX non-root mode and use them in VMX root mode (our pool manager approach).

### Can I call printk in VMX root mode?

Yes, `printk` is generally safe because it writes to a ring buffer and doesn't sleep. However, it may be slow. For high-frequency VM-exits, use `printk_ratelimited()` or the custom log buffer to avoid flooding.

### Why do we need INVVPID? Isn't INVEPT enough?

INVEPT invalidates EPT-derived translations (GPA → HPA). INVVPID invalidates guest-linear translations (GVA → GPA). These are different translation caches. When the guest changes CR3, you need to invalidate the GVA → GPA TLB. INVEPT only covers the EPT layer. With VPID, you can selectively flush one guest's TLB without affecting others or the host.

### Can I receive interrupts in VMX root mode?

No. On VM-exit, EFLAGS.IF is cleared (interrupts disabled). The VM-exit handler runs with interrupts off. If an interrupt fires during this time, it's held pending until VMRESUME. This is why we can't call functions that rely on interrupt-driven mechanisms.

### What about RDTSC exiting?

On some CPUs, RDTSC exiting is a "must-be-1" control bit (it's forced on by the processor capability MSR). You MUST handle EXIT_REASON_RDTSC in your VM-exit handler. Our handler from Part 7 simply advances RIP — this is correct because RDTSC is a "trap-type" exit (the instruction executed, we emulate its result or let the hardware handle it).

For RDTSC, the actual TSC value is already in EDX:EAX from the CPU (the instruction executed before the VM-exit). If you don't modify EDX:EAX and just advance RIP, the guest sees the correct TSC value. If you want to offset or virtualize the TSC, write to `TSC_OFFSET` in the VMCS and read the adjusted value.

---

## Let's Test It!

### Testing Event Injection and Exception Bitmap

```c
/* file: module/vmx.c — called from an IOCTL handler after VMLAUNCH */
static void TestBreakpointInterception(void) {
    /*
     * Set exception bitmap to intercept #BP (breakpoint, vector 3).
     * After this, every INT3 in the guest will cause a VM-exit.
     */
    uint64_t bitmap = (1 << EXCEPTION_VECTOR_BREAKPOINT);
    vmwrite(EXCEPTION_BITMAP, bitmap);

    printk(KERN_INFO "[*] Hyperion: breakpoint interception enabled\n");

    /* Now trigger a breakpoint from guest code:
     *   asm volatile("int3");
     * This will cause a VM-exit, the handler will log it, and
     * re-inject the #BP so the guest can handle it normally. */
}
```

### Testing Hidden Hooks

Test read/write hook on a kernel data structure:

```c
/* file: module/vmx.c — example test for read/write hook */
static void TestReadWriteHook(void) {
    /*
     * Hook the current task_struct for read/write monitoring.
     * Every time the kernel reads or writes current->pid, etc.,
     * our hook will trigger and log the access.
     */
    void *Target = (void *)current;

    EptPerformPageHook(Target, NULL, NULL, true, true, false);

    printk(KERN_INFO "[*] Hyperion: read/write hook on current "
                     "task_struct (0x%px)\n", Target);

    /* After this, reading/writing to fields of 'current' will
     * cause EPT violations that our handler logs and re-enables. */
}
```

Test execute hook on a kernel function:

```c
/* file: module/vmx.c — example test for hidden execute hook */
/* Our hook function for kfree */
static void KfreeHook(const void *ptr) {
    printk(KERN_INFO "[*] Hyperion: kfree(0x%px) called\n", ptr);

    /* Call the original kfree */
    if (OriginalKfree)
        OriginalKfree(ptr);
}

static void (*OriginalKfree)(const void *) = NULL;

static void TestHiddenExecuteHook(void) {
    /*
     * Hidden execute hook on kfree.  All kfree() calls will be
     * redirected through our hook function, which logs the freed
     * pointer before calling the original implementation.
     */
    void *Target = (void *)kfree;

    EptPerformPageHook(Target, KfreeHook,
                       (void **)&OriginalKfree,
                       false, false, true);

    printk(KERN_INFO "[*] Hyperion: hidden execute hook on kfree\n");
}
```

### Testing Syscall Hook

```c
/* file: module/vmx.c — syscall hook test */
static void TestSyscallHook(void) {
    SyscallHookFindTable();

    void *Target = SyscallHookGetFunctionAddress(__NR_openat);
    if (!Target) {
        pr_err("[*] Hyperion: sys_openat not found\n");
        return;
    }

    /*
     * Hidden execute hook on __x64_sys_openat.
     * Every openat() syscall will trigger our hook, which logs
     * the filename being opened.
     */
    EptPerformPageHook(Target, SyscallOpenatHook,
                       (void **)&OriginalOpenat,
                       false, false, true);

    printk(KERN_INFO "[*] Hyperion: syscall hook on openat (0x%px)\n",
           Target);
}
```

### Expected Output

When the hooks are active and the system runs, you should see:

```
[*] Hyperion: hidden hook trigger — RIP=0xffffffff81234567 phys=0x12345000 READ
[*] Hyperion: hidden hook trigger — RIP=0xffffffff81234567 phys=0x12345000 WRITE
[*] Hyperion: kfree(0xffff888123456000) called
[*] Hyperion: openat("/etc/passwd", flags=0x80000)
```

---

## Conclusion

In this part, we implemented several powerful hypervisor features: event injection to control what interrupts the guest sees, the exception bitmap for intercepting specific exceptions, the Monitor Trap Flag for single-stepping invisibly, hidden hooks using EPT for unlimited read/write/execute monitoring, syscall hooks via the sys_call_table, VPID-based TLB management for performance, and a custom message-passing system for communicating between VMX root mode and user space.

These features form the foundation for building practical hypervisor-based tools: debuggers, security monitors, syscall tracers, and more. The EPT-based hooks are especially powerful because they operate below the OS — the guest cannot detect or bypass them without escaping the hypervisor itself.
