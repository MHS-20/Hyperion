# Hypervisor From Scratch — Part 7: Using EPT & Page-Level Monitoring Features

This is the 7th part of the tutorial *Hypervisor From Scratch*, and it's about using the Extended Page Table (EPT) in an already running system. As you might know, paging is an essential part of managing memory on modern operating systems. Hypervisors use an additional paging table; this gives us an excellent opportunity to monitor different aspects of memory (Read-Write-Execute) without modifying the operating system's page-tables. EPT is a hardware mechanism, so it's fast, but on the other hand, we have to deal with different caching and synchronization problems.

This part is highly dependent on Part 4 — *Address Translation Using Extended Page Table (EPT)*, so please read that part one more time; thus, I avoid redescribing the basic concepts relating to EPT Tables.

In the 7th part, we'll see how we can virtualize our currently running system by configuring VMCS and creating identity tables based on Memory Type Range Register (MTRR) then we use monitoring features to detect the execution of some kernel functions.

> **What is EPT and why should you care?**  
> If you're new to hardware virtualization, EPT (Extended Page Table) is Intel's implementation of Second-Level Address Translation (SLAT). Normally, when a CPU accesses memory, it walks the guest's page tables to translate virtual addresses to physical addresses. With EPT enabled, there is a *second* translation step: the guest-physical address produced by the first walk is then translated through the EPT to produce the real host-physical address. This gives the hypervisor a transparent interception point for *all* memory accesses — without the guest ever knowing.

Throughout this part, functions starting with `Hv` are hypervisor routines that you call from IOCTL handlers; avoid calling methods with `Vmx` prefix directly as these functions manage the operations relating to VMX operations. Functions with `Ept` prefix relate to Extended Page Table (EPT). Functions with `Vmcall` prefix are for VMCALL services, and functions with `Invept` prefix are related to invalidating EPT caches.

---

## Implementing Functions to Manage VMCALLs

We start this article by implementing functions relating to VMCALL. Intel describes VMCALL as *"Call to VM monitor by causing VM exit."*

VMCALL allows guest software to call for service into an underlying VM monitor. The details of the programming interface for such calls are VMM-specific. This instruction does nothing more than cause a VM exit.

In other words, whenever you execute a VMCALL instruction in VMX non-root mode (whenever a VM-exit occurs, we are in VMX root-mode, and we stay in VMX root mode until we execute VMRESUME or VMXOFF, so any other context is VMX non-root mode — meaning other kernel code can use VMCALL in their contexts to request a service from our hypervisor in VMX root mode).

Execution of VMCALL causes a VM-exit (`EXIT_REASON_VMCALL`). As we can set registers and stack before execution of VMCALL, we can send parameters to the VMCALL handler. All we need to do is design a calling convention so that both the VMCALL handler and the code which requests a service can work together perfectly.

> **Calling conventions on Linux**  
> On Linux x86_64, the System V AMD64 ABI is used. The first six integer/pointer arguments are passed in registers `RDI`, `RSI`, `RDX`, `RCX`, `R8`, `R9` (in that order), and the return value is in `RAX`. This is different from the Windows x64 convention which uses `RCX`, `RDX`, `R8`, `R9`. We design our VMCALL interface around the System V ABI so that the guest's register state at the time of the VMCALL instruction maps cleanly to C function parameters.

The first thing we need to implement is a function in assembly which executes VMCALL and returns.

```c
/* file: module/exit_handler.S */
.globl AsmVmxVmcall
AsmVmxVmcall:
    vmcall
    ret
```

It's declared in C like this:

```c
/* file: module/vmx.h */
extern uint64_t AsmVmxVmcall(uint64_t VmcallNumber, uint64_t OptionalParam1,
                             uint64_t OptionalParam2, uint64_t OptionalParam3);
```

What distinguishes the above code is that we're not modifying anything in `AsmVmxVmcall`. If someone passes parameters to it, the parameters are in `RDI`, `RSI`, `RDX`, `RCX` (System V ABI), and those registers are left untouched when `vmcall` executes, so the VM-exit handler can read them from the saved guest register state.

As we save all the registers on VM-exit, in the VM-exit handler we pass `GuestRegs->rdi`, `GuestRegs->rsi`, `GuestRegs->rdx`, `GuestRegs->rcx` to the VMCALL handler. `RDI` is the VMCALL number which specifies the service we want our hypervisor to perform, and `RSI`, `RDX`, `RCX` are optional parameters.

```c
/* file: module/vmx.c — inside main_vmexit_handler() */
case EXIT_REASON_VMCALL:
{
    GuestRegs->rax = VmxVmcallHandler(GuestRegs->rdi, GuestRegs->rsi,
                                      GuestRegs->rdx, GuestRegs->rcx);
    resume_to_next_instruction();
    break;
}
```

We also need to define the exit reason. It should already be in `vmx.h`:

```c
/* file: module/vmx.h */
#define EXIT_REASON_VMCALL 18
```

For example, we have the following services (VMCALL numbers) for our hypervisor in this part:

```c
/* file: module/hyperion.h */
#define VMCALL_TEST              0x1  /* Test VMCALL                          */
#define VMCALL_VMXOFF            0x2  /* Call VMXOFF to turn off the hypervisor*/
#define VMCALL_EXEC_HOOK_PAGE    0x3  /* Hook: clear Execute Access in EPT    */
#define VMCALL_INVEPT_ALL_CONTEXT     0x4  /* Invalidate EPT (all contexts)   */
#define VMCALL_INVEPT_SINGLE_CONTEXT  0x5  /* Invalidate EPT (single context) */
```

There is nothing special for `VmxVmcallHandler` — it's just a simple switch-case:

```c
/* file: module/vmx.c */
int VmxVmcallHandler(uint64_t VmcallNumber, uint64_t OptionalParam1,
                     uint64_t OptionalParam2, uint64_t OptionalParam3)
{
    int VmcallStatus = -1;

    switch (VmcallNumber)
    {
    case VMCALL_TEST:
    {
        VmcallStatus = VmcallTest(OptionalParam1, OptionalParam2, OptionalParam3);
        break;
    }
    case VMCALL_VMXOFF:
    {
        VmxVmxoff();
        VmcallStatus = 0;
        break;
    }
    case VMCALL_EXEC_HOOK_PAGE:
    {
        if (EptVmxRootModePageHook(OptionalParam1, true))
            VmcallStatus = 0;
        break;
    }
    case VMCALL_INVEPT_SINGLE_CONTEXT:
    {
        InveptSingleContext(OptionalParam1);
        VmcallStatus = 0;
        break;
    }
    case VMCALL_INVEPT_ALL_CONTEXT:
    {
        InveptAllContexts();
        VmcallStatus = 0;
        break;
    }
    default:
    {
        printk(KERN_WARNING "[*] Hyperion: unsupported VMCALL 0x%llx\n",
               VmcallNumber);
        VmcallStatus = -1;
        break;
    }
    }
    return VmcallStatus;
}
```

For testing it, I created a function called `VmcallTest` — it simply shows the parameters passed to the VMCALL:

```c
/* file: module/vmx.c */
static int VmcallTest(uint64_t Param1, uint64_t Param2, uint64_t Param3)
{
    printk(KERN_INFO "[*] Hyperion: VmcallTest called with "
                     "Param1=0x%llx Param2=0x%llx Param3=0x%llx\n",
           Param1, Param2, Param3);
    return 0;
}
```

Finally, we can use the following piece of code from VMX non-root mode (for example, from an IOCTL handler) and pass `VMCALL_TEST` as the VMCALL number along with other optional parameters:

```c
/* file: module/driver.c — inside an IOCTL handler, after VMX is launched */
AsmVmxVmcall(VMCALL_TEST, 0x22, 0x333, 0x4444);
```

Don't forget that the above code should only be executed in VMX non-root mode (i.e., after VMLAUNCH has succeeded).

There is nothing more I can say about VMCALL, but for further reading: if you want to know what happens if you execute VMCALL in VMX root-mode, it invokes an SMM monitor. This invocation will activate the dual-monitor treatment of system-management interrupts (SMIs) and system-management mode (SMM) if it is not already active. In other words, executing VMCALL in VMX root mode causes an SMM VM exit! Read Section 34.15.2 and Section 34.15.6 in Intel SDM for more information.

---

## Starting with MMU Virtualization (EPT)

Let me start with the differences between physical and virtual addressing.

**Physical addressing** means that your program knows the real layout of RAM. When you access a variable at address `0x8746b3`, that's where it's stored in the physical RAM chips.

With **virtual addressing**, all application memory accesses go to a page table, which then maps from the virtual to the physical address. So every application has its own "private" address space, and no program can read or write to another program's memory.

EPT is a page table with a page-walk length of 4 (or in newer versions 5). It translates **guest-physical addresses** to **host-physical addresses**.

> **Why EPT maps physical, not virtual**  
> First, you have to understand that EPT maps guest physical pages to host physical pages. Mapping physical addresses makes hypervisors much easier to understand because you can forget about all the concepts relating to virtual memory and the operating system's memory manager. Why? Because you cannot allocate more physical memory. Sure, you can hot-plug RAM into the motherboard, but let's forget about that for now. The RAM usually starts at address 0 and usually ends at `AMOUNT_OF_RAM + SOME_MORE`, where `SOME_MORE` is MMIO/device space.

Note the holes between ranges (e.g., `A0000 - 100000`); the ranges are backed by actual physical RAM, and the holes are the MMIO space — regions that the BIOS maps to device I/O rather than real memory.

By now, you know that if you allocate or free memory, the RAM ranges are always present and what changes is the content of data in the RAM.

Keep in mind, there are certainly no holes in the RAM as an electronic circuit, but it's how BIOS maps certain physical memory ranges to the actual hardware RAM. In other words, RAM usually isn't one contiguous address space — if you have 1 GB of RAM it's often not one single piece of `0 … 1GB` physical address space, but some parts of that space belong to, e.g., network card, audio card, USB hub, etc.

Let's see what hypervisors like KVM, QEMU, VirtualBox do with physical memory. We don't have the same approach, but it helps you understand MMU virtualization better.

In QEMU/KVM, the VM has its own physical memory, and our host also has some physical address space. EPT exists so that you can translate the guest physical memory to host physical memory. For example, if a guest wants to read from physical address `0x1000`, it looks into EPT, and EPT tells it that the content of the memory is on the host's physical address `0x5000`. You certainly do not want to let some guests read physical memory on the host, so it's the hypervisor's job to set up EPTs correctly and have some chunk of physical memory dedicated to a guest.

---

## Memory Type Range Register (MTRR)

By now, you have some idea about how memory (RAM) is divided into regions; these regions can be found using MTRR registers — that's all!

Now let's explain them more precisely.

> **What are MTRRs?**  
> Memory Type Range Registers (MTRRs) are a set of processor supplementary capability control registers that provide system software with control of how accesses to memory ranges by the CPU are cached. It uses a set of programmable model-specific registers (MSRs), which are special registers provided by most modern CPUs. Possible access modes to memory ranges can be: uncached, write-through, write-combining, write-protect, and write-back. In write-back mode, writes are written to the CPU's cache, and the cache is marked dirty so that its contents are written to memory later.

In old x86 architecture systems, mainly where separate chips provided the cache outside of the CPU package, this function was controlled by the chipset itself and configured through BIOS settings. When the CPU cache was moved inside the CPU, the CPUs implemented fixed-range MTRRs.

Typically, the BIOS configures the MTRRs. The operating system or executive is then free to modify the memory map using the typical page-level cacheability attributes.

If you're confused by the above sentences, let me explain it more clearly. RAM is divided into different regions. We want to read the details (Base Address, End Address, and Cache Policy) of these chunks using MTRR registers. Cache policy is something that BIOS or the operating system sets for a particular region. For example, the OS decides to put UC (uncacheable) to a region that starts from `0x1000` to `0x2000` (physical address) of RAM, then it chooses to put WB (write-back) to a region starting from `0x5000` to `0x7000` (physical address). It's based on OS policy.

> **Why does caching policy matter for a hypervisor?**  
> If you get the caching policy wrong in your EPT tables, the consequences are catastrophic. For example, devices that use physical memory as a command-and-control mechanism (like the APIC) will go through the cache and won't immediately respond to requests, or real-time interrupts won't work. The EPT memory type for each page must match what the system expects based on MTRRs, otherwise you get silent data corruption or system hangs.

OK, let's see how to read these MTRRs.

The availability of the MTRR feature is model-specific — we can determine if MTRRs are supported on a processor by executing the CPUID instruction and reading the state of the MTRR flag (bit 12) in the feature information register (EDX). This check is not essential as our processor probably supports it since it's an old feature.

What is essential for us is an MSR called `IA32_MTRR_DEF_TYPE`. The following structure represents it:

```c
/* file: module/ept.h */
typedef union {
    uint64_t flags;
    struct {
        uint64_t default_memory_type : 3;  /* bits 2:0  — default memory type   */
        uint64_t reserved1           : 7;  /* bits 9:3                         */
        uint64_t fixed_range_enable  : 1;  /* bit  10   — fixed-range MTRR en.  */
        uint64_t mtrr_enable         : 1;  /* bit  11   — MTRR enable            */
        uint64_t reserved2           : 52; /* bits 63:12                        */
    } fields;
} IA32_MTRR_DEF_TYPE_REGISTER;
```

We implement a function called `EptCheckFeatures` which checks whether our processor supports basic EPT features or not; for MTRRs, we check whether MTRRs are enabled or not. Having MTRRs enabled is necessary for our hypervisor.

```c
/* file: module/ept.c */
static bool EptCheckFeatures(void)
{
    IA32_MTRR_DEF_TYPE_REGISTER MTRRDefType;

    rdmsrl(MSR_IA32_MTRR_DEF_TYPE, MTRRDefType.flags);

    if (!MTRRDefType.fields.mtrr_enable) {
        pr_err("[*] Hyperion: MTRR dynamic ranges not supported\n");
        return false;
    }

    /* Also check EPT support via CPUID */
    uint32_t eax, ebx, ecx, edx;
    eax = 1;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(eax)
                 : "memory");

    if (!(ecx & (1 << 5))) {
        pr_err("[*] Hyperion: VMX not supported\n");
        return false;
    }

    return true;
}
```

### Building MTRR Map

In order to read MTRRs, we start by reading the VCNT value of `IA32_MTRRCAP` MSR (`0xFE`), which determines the number of variable MTRRs (number of regions).

> **How MTRR variable ranges work**  
> Each variable MTRR consists of a pair of MSRs: a PHYSBASE and a PHYSMASK. The PHYSBASE contains the base physical address and the memory type. The PHYSMASK contains a mask that defines the size of the region and a "valid" bit. The mask works like this: the lowest set bit in the mask's address field determines the size of the region. All bits above that must be set in the mask, and all bits below it must be clear.

The next step is to iterate through each MTRR variable; we read `MSR_IA32_MTRR_PHYSBASE0` and `MSR_IA32_MTRR_PHYSMASK0` for each range and check if the range is valid or not.

```c
/* file: module/ept.c — inside EptBuildMtrrMap() */
CurrentPhysBase.flags = rdmsrl(MSR_IA32_MTRR_PHYSBASE0 + (CurrentRegister * 2),
                               CurrentPhysBase.flags);
CurrentPhysMask.flags = rdmsrl(MSR_IA32_MTRR_PHYSMASK0 + (CurrentRegister * 2),
                               CurrentPhysMask.flags);
```

> **Note on `rdmsrl`**  
> `rdmsrl(msr, val)` is a Linux kernel helper that reads a 64-bit MSR into `val`. It wraps the `RDMSR` instruction. Similarly, `wrmsrl(msr, val)` writes a value to an MSR.

Now we need to calculate the start address and the end address (physical) based on MSRs.

The start address:

```c
/* file: module/ept.c — inside EptBuildMtrrMap() */
/* Calculate the base address in bytes */
Descriptor->PhysicalBaseAddress = CurrentPhysBase.fields.page_frame_number * PAGE_SIZE;
```

The end address:

```c
/* file: module/ept.c — inside EptBuildMtrrMap() */
/* The lowest bit of the mask that is set to 1 specifies the size of the range.
 * We use __builtin_ctzll to find the lowest set bit (equivalent to BitScanForward64).
 */
uint64_t mask_aligned = CurrentPhysMask.fields.page_frame_number * PAGE_SIZE;
int NumberOfBitsInMask = __builtin_ctzll(mask_aligned);

/* Size of the range in bytes + Base Address */
Descriptor->PhysicalEndAddress =
    Descriptor->PhysicalBaseAddress + ((1ULL << NumberOfBitsInMask) - 1ULL);
```

> **What is `__builtin_ctzll`?**  
> `__builtin_ctzll(x)` is a GCC/Clang built-in that returns the number of trailing zero bits in a 64-bit value. This is the Linux equivalent of `_BitScanForward64` on MSVC. It tells us which bit position is the lowest set bit — which corresponds to the power-of-two size of the MTRR range.

And finally, read the cache policy which is set by either BIOS or the operating system:

```c
/* file: module/ept.c — inside EptBuildMtrrMap() */
/* Memory type (cacheability attributes) */
Descriptor->MemoryType = (uint8_t)CurrentPhysBase.fields.type;
```

We need a structure to hold each MTRR range and a global state for EPT. Let's define them:

```c
/* file: module/ept.h */
typedef struct _MTRR_RANGE_DESCRIPTOR {
    uint64_t PhysicalBaseAddress;
    uint64_t PhysicalEndAddress;
    uint8_t  MemoryType;
} MTRR_RANGE_DESCRIPTOR;

#define MEMORY_TYPE_UNCACHEABLE  0
#define MEMORY_TYPE_WRITE_COMBINING 1
#define MEMORY_TYPE_WRITE_THROUGH 4
#define MEMORY_TYPE_WRITE_PROTECT 5
#define MEMORY_TYPE_WRITE_BACK   6

#define MAX_MTRR_RANGES 64

typedef struct _EPT_STATE {
    MTRR_RANGE_DESCRIPTOR MemoryRanges[MAX_MTRR_RANGES];
    uint32_t NumberOfEnabledMemoryRanges;
    struct VMM_EPT_PAGE_TABLE *EptPageTable;
    EPTP EptPointer;
} EPT_STATE;

extern EPT_STATE *g_ept_state;
```

```c
/* file: module/ept.c */
EPT_STATE *g_ept_state = NULL;
```

We also need the MTRR PHYSBASE and PHYSMASK register layouts:

```c
/* file: module/ept.h */
typedef union {
    uint64_t flags;
    struct {
        uint64_t type             : 8;   /* bits 7:0  — memory type            */
        uint64_t reserved1        : 4;   /* bits 11:8                        */
        uint64_t page_frame_number: 40;  /* bits 51:12 — phys base / mask      */
        uint64_t reserved2        : 12;  /* bits 63:52                        */
    } fields;
} IA32_MTRR_PHYSBASE_REGISTER;

typedef union {
    uint64_t flags;
    struct {
        uint64_t valid            : 1;   /* bit  0  — range is valid           */
        uint64_t reserved1        : 11;  /* bits 11:1                        */
        uint64_t page_frame_number: 40;  /* bits 51:12 — phys addr mask        */
        uint64_t reserved2        : 12;  /* bits 63:52                        */
    } fields;
} IA32_MTRR_PHYSMASK_REGISTER;

typedef union {
    uint64_t flags;
    struct {
        uint64_t variable_range_count : 8;  /* bits 7:0   — # of variable MTRRs */
        uint64_t fixed_range_support  : 1;  /* bit  8                            */
        uint64_t reserved1            : 55; /* bits 63:9                         */
    } fields;
} IA32_MTRR_CAPABILITIES_REGISTER;
```

Putting it all together, we have the following function:

```c
/* file: module/ept.c */
static bool EptBuildMtrrMap(void)
{
    IA32_MTRR_CAPABILITIES_REGISTER MTRRCap;
    IA32_MTRR_PHYSBASE_REGISTER CurrentPhysBase;
    IA32_MTRR_PHYSMASK_REGISTER CurrentPhysMask;
    MTRR_RANGE_DESCRIPTOR *Descriptor;
    uint32_t CurrentRegister;
    int NumberOfBitsInMask;

    rdmsrl(MSR_IA32_MTRR_CAPABILITIES, MTRRCap.flags);

    for (CurrentRegister = 0;
         CurrentRegister < MTRRCap.fields.variable_range_count;
         CurrentRegister++)
    {
        /* For each dynamic register pair */
        rdmsrl(MSR_IA32_MTRR_PHYSBASE0 + (CurrentRegister * 2),
               CurrentPhysBase.flags);
        rdmsrl(MSR_IA32_MTRR_PHYSMASK0 + (CurrentRegister * 2),
               CurrentPhysMask.flags);

        /* Is the range enabled? */
        if (CurrentPhysMask.fields.valid)
        {
            Descriptor = &g_ept_state->MemoryRanges[
                g_ept_state->NumberOfEnabledMemoryRanges++];

            /* Calculate the base address in bytes */
            Descriptor->PhysicalBaseAddress =
                CurrentPhysBase.fields.page_frame_number * PAGE_SIZE;

            /* Calculate the total size of the range */
            uint64_t mask_aligned =
                CurrentPhysMask.fields.page_frame_number * PAGE_SIZE;
            NumberOfBitsInMask = __builtin_ctzll(mask_aligned);

            /* Size of the range in bytes + Base Address */
            Descriptor->PhysicalEndAddress =
                Descriptor->PhysicalBaseAddress +
                ((1ULL << NumberOfBitsInMask) - 1ULL);

            /* Memory type (cacheability attributes) */
            Descriptor->MemoryType = (uint8_t)CurrentPhysBase.fields.type;

            if (Descriptor->MemoryType == MEMORY_TYPE_WRITE_BACK)
            {
                /* This is already our default, so no need to store this range.
                 * Simply 'free' the range we just wrote. */
                g_ept_state->NumberOfEnabledMemoryRanges--;
            }

            printk(KERN_INFO "[*] Hyperion: MTRR Range: Base=0x%llx "
                             "End=0x%llx Type=0x%x\n",
                   Descriptor->PhysicalBaseAddress,
                   Descriptor->PhysicalEndAddress,
                   Descriptor->MemoryType);
        }
    }

    printk(KERN_INFO "[*] Hyperion: Total MTRR Ranges Committed: %d\n",
           g_ept_state->NumberOfEnabledMemoryRanges);

    return true;
}
```

For further information about the calculation of MTRRs, you can read Intel SDM Vol 3A, Section 11.11.3 — *Example Base and Mask Calculations*.

### Fixed-Range MTRRs and PAT

The above section is enough for understanding the MTRRs for EPT. I want to talk a little more about physical and virtual memory layout and caching policy (you can skip this section as it does not directly relate to our hypervisor).

There are other MTRR registers called Fixed-Range Registers. As the name implies, these registers are predefined ranges defined by the processor:

| Range          | Register               |
|----------------|------------------------|
| 00000–7FFFF    | IA32_MTRR_FIX64K_00000 |
| 80000–9FFFF    | IA32_MTRR_FIX16K_80000 |
| A0000–BFFFF    | IA32_MTRR_FIX16K_A0000 |
| C0000–C7FFF    | IA32_MTRR_FIX4K_C0000  |
| C8000–CFFFF    | IA32_MTRR_FIX4K_C8000  |
| ...            | ...                    |

As you can see, the start of physical RAM is defined by these fixed range registers, which exist for performance and legacy reasons.

Note that MTRRs should be defined contiguously; if your MTRRs are not contiguous, then the rest of the RAM is typically assumed as a hole.

Keep in mind that caching policy for each region of RAM is defined by MTRRs for **physical** regions and by the Page Attribute Table (PAT) for **virtual** regions, so each page can use its own caching policy by configuring the `IA32_PAT` MSR. This means that sometimes the caching policy specified in MTRR registers is ignored, and instead a page-level cache policy is used. There is a table in Intel SDM that shows the precedence rules between PAT and MTRRs (Table 11-7: *Effective Page-Level Memory Types*).

For further reading, see Intel SDM Vol 3A, Chapter 11 — Sections 11.11 (MTRRs) and 11.12 (PAT).

---

## Virtualizing Current System's Memory Using EPT

As you have some previous information from EPT (Part 4), we create an EPT table for our VM. In the case of fully virtualizing the memory of the current machine, there are different approaches in implementing EPT; we can either have a separate EPT table for each of the cores or one EPT table for all the cores. Our approach is using one EPT for all the cores as it's simpler to implement and manage.

What we are trying to do is creating an EPT table that maps all of the available physical memory (we have the details of physical memory from MTRRs) to the physical address. It's like adding a table that maps each address to itself (identity mapping), but with additional fields to control access permissions.

### EPT Identity Mapping

In our hypervisor — or all hypervisors that virtualize an already running system (not KVM/QEMU etc.) — we have a term called **identity mapping** or **1:1 mapping**. It means that if you access guest PA (physical address) `0x4000`, it will access host PA at `0x4000`. Thus, you have to map RAM's holes as well as memory ranges to the guest.

It is the same as regular page tables (you can set page tables that way as well, so that virtual address `0x1234` corresponds to the physical address `0x1234`).

If you don't map some physical memory and the guest accesses it, then you'll get an **EPT Violation**, which can be understood as the hypervisor's page fault.

> **Why 2 MB pages initially?**  
> In order to map everything one by one, we'll create PML4Es, then PDPTEs, then PDEs, and finally PTEs. In cases with 2 MB of granularity, we skip PTEs. 4 KB granularity is preferred for fine-grained monitoring, but 4 GB of RAM results in one million 4 KB pages — having 4 KB granularity will eat a lot of memory and take quite some time to set up, which will drive you crazy if you test your hypervisor frequently. The approach used by most self-virtualizing hypervisors is to initially set up 2 MB pages for the whole system (including RAM ranges and MMIO holes) and then break some 2 MB pages into 4 KB pages as needed.

After splitting to 4 KB pages, you can merge them back to 2 MB pages again. We do the same: first initialize with 2 MB granularity, then split to 4 KB whenever needed.

> **Why don't we care about new memory allocations of the kernel?**  
> That's because we mapped all of the physical memory (every possible address in physical RAM) using 2 MB chunks, including those which are allocated and those which are not allocated yet. So no matter if the kernel allocates a new memory chunk, we already have it in our EPT table.

What we want to do is create a PML4E, then a PDPTE; we add that PDPTE into the PML4E. Then we create a PDE and add it to the PDPTE. Finally, we create PTEs which point to physical addresses. We create 512 entries in each table (the maximum number of entries in all paging structures including EPT page tables and regular page tables is 512).

All in all, our hypervisor should not care about any virtual address — it's all about physical memory.

Let's implement it!

### Setting up PML4 and PML3 Entries

First, we need to define the structures for our EPT page table. We need types that combine all four levels plus a dynamic split structure for 4 KB page splitting:

```c
/* file: module/ept.h */
#define VMM_EPT_PML4E_COUNT 512
#define VMM_EPT_PML3E_COUNT 512
#define VMM_EPT_PML2E_COUNT 512
#define VMM_EPT_PML1E_COUNT 512
#define SIZE_2_MB (2 * 1024 * 1024ULL)

/* PML2 entry when used as a 2 MB large page */
typedef union _EPT_PML2_ENTRY {
    uint64_t all;
    struct {
        uint64_t read           : 1;   /* bit 0  */
        uint64_t write          : 1;   /* bit 1  */
        uint64_t execute        : 1;   /* bit 2  */
        uint64_t reserved1      : 5;   /* bits 7:3  */
        uint64_t accessed       : 1;   /* bit 8  */
        uint64_t ignored1       : 1;   /* bit 9  */
        uint64_t execute_for_usermode : 1; /* bit 10 */
        uint64_t ignored2       : 1;   /* bit 11 */
        uint64_t memory_type    : 3;   /* bits 14:12 — EPT memory type  */
        uint64_t ignore_pat     : 1;   /* bit 15 */
        uint64_t large_page     : 1;   /* bit 16 — must be 1 for 2 MB pages */
        uint64_t reserved2      : 4;   /* bits 20:17 */
        uint64_t page_frame_number : 26; /* bits 46:21 — PFN (for 2 MB pages,
                                          * this is the physical address >> 21) */
        uint64_t reserved3      : 17;  /* bits 63:47 */
    } fields;
} EPT_PML2_ENTRY;

/* PML2 entry when used as a pointer to a PML1 table (after splitting) */
typedef union _EPT_PML2_POINTER {
    uint64_t all;
    struct {
        uint64_t read           : 1;
        uint64_t write          : 1;
        uint64_t execute        : 1;
        uint64_t reserved1      : 5;
        uint64_t accessed       : 1;
        uint64_t ignored1       : 1;
        uint64_t execute_for_usermode : 1;
        uint64_t ignored2       : 1;
        uint64_t physical_address : 36; /* bits 47:12 — PFN of PML1 table */
        uint64_t reserved2      : 4;
        uint64_t ignored3       : 12;
    } fields;
} EPT_PML2_POINTER;

/* PML1 entry — 4 KB page */
typedef union _EPT_PML1_ENTRY {
    uint64_t all;
    struct {
        uint64_t read           : 1;
        uint64_t write          : 1;
        uint64_t execute        : 1;
        uint64_t reserved1      : 5;
        uint64_t accessed       : 1;
        uint64_t dirty          : 1;
        uint64_t execute_for_usermode : 1;
        uint64_t ignored1       : 1;
        uint64_t memory_type    : 3;
        uint64_t ignore_pat     : 1;
        uint64_t ignored2       : 1;
        uint64_t physical_address : 36; /* bits 47:12 — PFN of 4 KB page */
        uint64_t reserved2      : 4;
        uint64_t ignored3       : 11;
        uint64_t suppress_ve    : 1;
    } fields;
} EPT_PML1_ENTRY;

/* Dynamic split: holds the PML1 table for a split 2 MB page */
typedef struct _VMM_EPT_DYNAMIC_SPLIT {
    EPT_PML1_ENTRY PML1[VMM_EPT_PML1E_COUNT];
    EPT_PML2_ENTRY *Entry;  /* back-pointer to the PML2 entry being split */
    struct list_head DynamicSplitList;  /* for linked-list tracking */
} VMM_EPT_DYNAMIC_SPLIT;

/* The full EPT page table structure */
typedef struct VMM_EPT_PAGE_TABLE {
    EPT_PML4E           PML4[VMM_EPT_PML4E_COUNT];
    EPT_PDPTE           PML3[VMM_EPT_PML3E_COUNT];
    EPT_PML2_ENTRY      PML2[VMM_EPT_PML3E_COUNT][VMM_EPT_PML2E_COUNT];
    struct list_head    DynamicSplitList;
} VMM_EPT_PAGE_TABLE;
```

Now, first of all, we have to allocate a large contiguous block of memory for our EPT page table and then zero it. On Linux, we use `__get_free_pages` for contiguous physical memory:

```c
/* file: module/ept.c — inside EptAllocateAndCreateIdentityPageTable() */
VMM_EPT_PAGE_TABLE *PageTable;

/* The EPT page table is large: 512*8 + 512*8 + 512*512*8 = ~2 MB
 * We need physically contiguous, page-aligned memory.
 * __get_free_pages returns a contiguous virtual region that is also
 * physically contiguous. */
int order = get_order(sizeof(VMM_EPT_PAGE_TABLE));
PageTable = (VMM_EPT_PAGE_TABLE *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, order);

if (PageTable == NULL) {
    pr_err("[*] Hyperion: failed to allocate memory for EPT page table\n");
    return NULL;
}
```

> **Why `__get_free_pages` instead of `kmalloc`?**  
> `kmalloc` works for smaller allocations, but the EPT page table is approximately 2 MB in size (512×8 + 512×8 + 512×512×8 bytes). While `kmalloc` can handle this on modern kernels, `__get_free_pages` guarantees that the allocation is physically contiguous and page-aligned, which is a requirement for EPT paging structures. The `__GFP_ZERO` flag ensures the memory is zeroed, equivalent to `memset(buffer, 0, size)`.

We have a linked list that holds the trace of every allocated split page; we have to initialize it first so we can de-allocate our allocated pages whenever we want to turn off our hypervisor.

```c
/* file: module/ept.c — inside EptAllocateAndCreateIdentityPageTable() */
INIT_LIST_HEAD(&PageTable->DynamicSplitList);
```

It's time to initialize the first table (EPT PML4). For the initialization phase, we set all the accesses to 1 (Read, Write, Execute) on all of the EPT tables.

The physical address (Page Frame Number — PFN) for the PML4E is PML3's address. Since the processor multiplies the PFN by `PAGE_SIZE` (4096) to get the physical address, we divide by `PAGE_SIZE`:

```c
/* file: module/ept.c — inside EptAllocateAndCreateIdentityPageTable() */
/* Mark the first 512 GB PML4 entry as present */
EPT_PML4E pml4e_template = {0};
pml4e_template.fields.read = 1;
pml4e_template.fields.write = 1;
pml4e_template.fields.execute = 1;
pml4e_template.fields.physical_address =
    virtual_to_physical(&PageTable->PML3[0]) / PAGE_SIZE;
PageTable->PML4[0] = pml4e_template;
```

Each PML4 entry covers 512 GB of memory, so one entry is more than enough. Each table has 512 entries, so we have to fill PML3 with 512 of 1 GB entries. We do this by creating a template with RWX enabled and filling the table with this template:

```c
/* file: module/ept.c — inside EptAllocateAndCreateIdentityPageTable() */
EPT_PDPTE pdpte_template = {0};
pdpte_template.fields.read = 1;
pdpte_template.fields.write = 1;
pdpte_template.fields.execute = 1;

/* Fill all 512 PML3 entries with the RWX template */
for (int EntryIndex = 0; EntryIndex < VMM_EPT_PML3E_COUNT; EntryIndex++) {
    EPT_PDPTE entry = pdpte_template;
    entry.fields.physical_address =
        virtual_to_physical(&PageTable->PML2[EntryIndex][0]) / PAGE_SIZE;
    PageTable->PML3[EntryIndex] = entry;
}
```

> **What does `virtual_to_physical` do?**  
> In the Linux kernel, virtual addresses in the linear mapping region can be converted to physical addresses by calling `virt_to_phys()` (or `__pa()`). Our `virtual_to_physical` wrapper does exactly this. This works because `__get_free_pages` returns memory from the direct-mapped region, where virtual-to-physical translation is a simple arithmetic offset.

For PML2, we have the same approach — fill it with an RWX template, but this time we set `LargePage` to 1 (for the reason about initialization with 2 MB granularity described above). We fill 512×512 entries since we have 512 PML3 entries, each of which points to 512 PML2 entries:

```c
/* file: module/ept.c — inside EptAllocateAndCreateIdentityPageTable() */
EPT_PML2_ENTRY pml2_template = {0};
pml2_template.fields.read = 1;
pml2_template.fields.write = 1;
pml2_template.fields.execute = 1;
pml2_template.fields.large_page = 1;

/* Fill all 512*512 PML2 entries with the RWX large-page template */
for (int EntryGroupIndex = 0; EntryGroupIndex < VMM_EPT_PML3E_COUNT; EntryGroupIndex++) {
    for (int EntryIndex = 0; EntryIndex < VMM_EPT_PML2E_COUNT; EntryIndex++) {
        EPT_PML2_ENTRY entry = pml2_template;
        EptSetupPML2Entry(&entry,
                         (EntryGroupIndex * VMM_EPT_PML2E_COUNT) + EntryIndex);
        PageTable->PML2[EntryGroupIndex][EntryIndex] = entry;
    }
}
```

### Setting up PML2 Entries

PML2 is different from the other tables; this is because, in our 2 MB design, it's the last table, so it has to deal with MTRRs' caching policy.

First, we have to set the `page_frame_number` of our PML2 entry. This is because we're mapping all 512 GB without any hole — we map every possible physical address within 512 GB:

```c
/* file: module/ept.c — inside EptSetupPML2Entry() */
/*
 * Each of the 512 collections of 512 PML2 entries is set up here.
 * This will, in total, identity map every physical address from 0x0 to
 * physical address 0x8000000000 (512 GB of memory).
 *
 * ((EntryGroupIndex * VMM_EPT_PML2E_COUNT) + EntryIndex) * 2MB is the
 * actual physical address we're mapping.
 */
NewEntry->fields.page_frame_number = PageFrameNumber;
```

Now it's time to see the actual caching policy based on MTRRs. Ranges in MTRRs are not divided by 4 KB or 2 MB — these are exact physical addresses. What we are going to do is iterate over each MTRR and see whether a particular MTRR describes our current physical address or not.

If none of them describe it, then we choose Write-Back (`MEMORY_TYPE_WRITE_BACK`) as the default caching policy; otherwise, we select the caching policy that is used in MTRRs.

This approach makes our EPT PML2 behave like a real system.

> **What happens if you get the caching policy wrong?**  
> If we don't choose the system-specific caching policy, it will cause catastrophic errors. For example, some devices that use physical memory as the command and control mechanism will go through the cache and won't immediately respond to our requests. The APIC (Advanced Programmable Interrupt Controller) device will not work correctly in the case of real-time interrupts. The processor uses MTRRs to decide how to cache each physical region, and our EPT must mirror those decisions.

```c
/* file: module/ept.c — inside EptSetupPML2Entry() */
uint64_t AddressOfPage = PageFrameNumber * SIZE_2_MB;
uint8_t TargetMemoryType = MEMORY_TYPE_WRITE_BACK;  /* default */

/* For each MTRR range */
for (uint32_t CurrentMtrrRange = 0;
     CurrentMtrrRange < g_ept_state->NumberOfEnabledMemoryRanges;
     CurrentMtrrRange++)
{
    /* If this page's address is below or equal to the max physical address
     * of the range */
    if (AddressOfPage <=
        g_ept_state->MemoryRanges[CurrentMtrrRange].PhysicalEndAddress)
    {
        /* And this page's last address is above or equal to the base
         * physical address of the range */
        if ((AddressOfPage + SIZE_2_MB - 1) >=
            g_ept_state->MemoryRanges[CurrentMtrrRange].PhysicalBaseAddress)
        {
            /* This page fell within one of the ranges specified by the
             * variable MTRRs. Mark this page with the same cache type. */
            TargetMemoryType =
                g_ept_state->MemoryRanges[CurrentMtrrRange].MemoryType;

            /* 11.11.4.1 MTRR Precedences: if UC, stop searching.
             * UC always takes precedence. */
            if (TargetMemoryType == MEMORY_TYPE_UNCACHEABLE)
                break;
        }
    }
}

/* Finally, commit the memory type to the entry. */
NewEntry->fields.memory_type = TargetMemoryType;
```

The full `EptSetupPML2Entry` function:

```c
/* file: module/ept.c */
static void EptSetupPML2Entry(EPT_PML2_ENTRY *NewEntry, uint64_t PageFrameNumber)
{
    uint64_t AddressOfPage = PageFrameNumber * SIZE_2_MB;
    uint8_t TargetMemoryType = MEMORY_TYPE_WRITE_BACK;

    /* Set the page frame number for identity mapping */
    NewEntry->fields.page_frame_number = PageFrameNumber;

    /* Find the correct memory type based on MTRR ranges */
    for (uint32_t i = 0; i < g_ept_state->NumberOfEnabledMemoryRanges; i++)
    {
        if (AddressOfPage <=
            g_ept_state->MemoryRanges[i].PhysicalEndAddress)
        {
            if ((AddressOfPage + SIZE_2_MB - 1) >=
                g_ept_state->MemoryRanges[i].PhysicalBaseAddress)
            {
                TargetMemoryType = g_ept_state->MemoryRanges[i].MemoryType;

                /* UC takes precedence — stop searching */
                if (TargetMemoryType == MEMORY_TYPE_UNCACHEABLE)
                    break;
            }
        }
    }

    NewEntry->fields.memory_type = TargetMemoryType;
}
```

Putting it all together:

```c
/* file: module/ept.c */
static VMM_EPT_PAGE_TABLE *EptAllocateAndCreateIdentityPageTable(void)
{
    VMM_EPT_PAGE_TABLE *PageTable;
    int order;

    /* Allocate the EPT page table as physically contiguous, zeroed memory */
    order = get_order(sizeof(VMM_EPT_PAGE_TABLE));
    PageTable = (VMM_EPT_PAGE_TABLE *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
                                                       order);
    if (!PageTable) {
        pr_err("[*] Hyperion: failed to allocate EPT page table\n");
        return NULL;
    }

    /* Initialize the dynamic split list */
    INIT_LIST_HEAD(&PageTable->DynamicSplitList);

    /* PML4[0] → PML3 (covers 512 GB) */
    EPT_PML4E pml4e = {0};
    pml4e.fields.read = 1;
    pml4e.fields.write = 1;
    pml4e.fields.execute = 1;
    pml4e.fields.physical_address =
        virtual_to_physical(&PageTable->PML3[0]) / PAGE_SIZE;
    PageTable->PML4[0] = pml4e;

    /* PML3[i] → PML2[i] (each covers 1 GB, 512 entries total) */
    EPT_PDPTE pdpte_template = {0};
    pdpte_template.fields.read = 1;
    pdpte_template.fields.write = 1;
    pdpte_template.fields.execute = 1;

    for (int i = 0; i < VMM_EPT_PML3E_COUNT; i++) {
        EPT_PDPTE entry = pdpte_template;
        entry.fields.physical_address =
            virtual_to_physical(&PageTable->PML2[i][0]) / PAGE_SIZE;
        PageTable->PML3[i] = entry;
    }

    /* PML2[i][j] — 2 MB large pages with MTRR-based memory types */
    EPT_PML2_ENTRY pml2_template = {0};
    pml2_template.fields.read = 1;
    pml2_template.fields.write = 1;
    pml2_template.fields.execute = 1;
    pml2_template.fields.large_page = 1;

    for (int group = 0; group < VMM_EPT_PML3E_COUNT; group++) {
        for (int idx = 0; idx < VMM_EPT_PML2E_COUNT; idx++) {
            EPT_PML2_ENTRY entry = pml2_template;
            EptSetupPML2Entry(&entry,
                              (group * VMM_EPT_PML2E_COUNT) + idx);
            PageTable->PML2[group][idx] = entry;
        }
    }

    return PageTable;
}
```

### EPT Violation

Intel describes EPT Violation like this:

> An EPT violation occurs when there is no EPT misconfiguration, but the EPT paging structure entries disallow access using the guest-physical address.

In short, every time an instruction tries to read a page (Read Access), or an instruction tries to write on a page (Write Access), or an instruction causes an instruction fetch from a page and the EPT attributes of that page don't allow this, then an EPT Violation occurs.

Let me explain a little more. Imagine we have an entry in our EPT table which is responsible for mapping physical address `0x1000`. In this entry, we set Write Access to 0 (Read Access = 1, Execute Access = 1). If any instruction tries to write on that page, for example by using `mov [0x1000], rax`, then as the paging attributes don't allow writing, an EPT Violation occurs and now our callback is called so that we can decide what to do with that page.

By `0x1000`, I mean a physical address. Of course, if you have the virtual address, it gets translated to a physical address first by the guest's page tables, and then the EPT translation applies.

Another example: let's assume a kernel function (for example `kfree`) is located at `ffffffff81234560`. If we convert it to a physical address, the address of `kfree` in physical memory might be `0x3B8000`. Now we find this physical address in our EPT PTE table and set Execute Access of that entry to 0. Now, each time someone tries to `call`, `jmp`, `ret`, etc. to this particular page, an EPT Violation occurs.

This is the basic idea of using EPT function hooks. We'll talk about it in detail in Part 8.

For now, first we have to read the physical address which caused this EPT Violation. It's done by reading `GUEST_PHYSICAL_ADDRESS` using the VMREAD instruction:

```c
/* file: module/vmx.c — inside the EPT violation handler */
uint64_t GuestPhysicalAddr = 0;
vmread(GUEST_PHYSICAL_ADDRESS, &GuestPhysicalAddr);
printk(KERN_INFO "[*] Hyperion: guest physical address: 0x%llx\n",
       GuestPhysicalAddr);
```

We need to make sure `GUEST_PHYSICAL_ADDRESS` is in our VMCS field enum:

```c
/* file: module/hyperion.h — in the enum vmcs_fields */
GUEST_PHYSICAL_ADDRESS = 0x00002400,
GUEST_PHYSICAL_ADDRESS_HIGH = 0x00002401,
```

The second thing that we have to read is Exit Qualification. If you remember from the previous parts, Exit Qualification gives additional details about exit reasons.

> **What is Exit Qualification?**  
> Each exit reason may have a special Exit Qualification that has a specific meaning for that particular exit reason. For EPT Violation, the Exit Qualification is a 64-bit value where each bit tells us something about what kind of access caused the violation: was it a read? a write? an instruction fetch? Was the page executable? etc.

Exit Reason can be read from `VM_EXIT_REASON`:

```c
/* file: module/vmx.c */
uint64_t exit_reason = 0;
vmread(VM_EXIT_REASON, &exit_reason);
```

In the case of EPT Violation, Exit Qualification shows the reason why this violation occurred. The following table shows the structure of Exit Qualification for EPT Violation:

| Bit  | Meaning                                      |
|------|----------------------------------------------|
| 0    | Read access caused the violation              |
| 1    | Write access caused the violation             |
| 2    | Instruction fetch caused the violation        |
| 3    | EPT read permission for the GPA was set       |
| 4    | EPT write permission for the GPA was set      |
| 5    | EPT execute permission for the GPA was set    |
| 6    | EPT read permission for the GPA (user-mode)   |
| 7    | EPT write permission for the GPA (user-mode)  |
| 8    | EPT execute permission for the GPA (user-mode)|
| 9    | GVA (guest virtual address) is valid          |
| 10-11| Reserved                                     |
| 12   | NMI unblocking due to IRET                    |
| 13-63| Reserved                                     |

We define a structure to parse this:

```c
/* file: module/vmx.h */
typedef union _VMX_EXIT_QUALIFICATION_EPT_VIOLATION {
    uint64_t all;
    struct {
        uint64_t read_access           : 1;  /* bit 0 — read caused violation  */
        uint64_t write_access          : 1;  /* bit 1 — write caused violation */
        uint64_t execute_access        : 1;  /* bit 2 — fetch caused violation */
        uint64_t ept_readable          : 1;  /* bit 3 — page was readable      */
        uint64_t ept_writable          : 1;  /* bit 4 — page was writable      */
        uint64_t ept_executable        : 1;  /* bit 5 — page was executable    */
        uint64_t ept_usermode_readable : 1;  /* bit 6                          */
        uint64_t ept_usermode_writable : 1;  /* bit 7                          */
        uint64_t ept_usermode_executable: 1;  /* bit 8                         */
        uint64_t gva_valid             : 1;  /* bit 9 — GVA is valid           */
        uint64_t reserved1             : 2;  /* bits 11:10                     */
        uint64_t nmi_unblocking        : 1;  /* bit 12                         */
        uint64_t reserved2             : 51; /* bits 63:13                     */
    } fields;
} VMX_EXIT_QUALIFICATION_EPT_VIOLATION;
```

Now that we have all the details, we need to pass them to the handler:

```c
/* file: module/vmx.c — inside main_vmexit_handler() */
case EXIT_REASON_EPT_VIOLATION:
{
    uint64_t exit_qual = 0;
    uint64_t guest_phys = 0;
    vmread(EXIT_QUALIFICATION, &exit_qual);
    vmread(GUEST_PHYSICAL_ADDRESS, &guest_phys);

    VMX_EXIT_QUALIFICATION_EPT_VIOLATION qual;
    qual.all = exit_qual;

    if (EptHandleEptViolation(qual, guest_phys))
    {
        /* Handled by page hook code */
    }
    else
    {
        printk(KERN_ERR "[*] Hyperion: unexpected EPT violation at "
                        "GPA=0x%llx qual=0x%llx\n", guest_phys, exit_qual);
        BUG();
    }
    break;
}
```

Make sure `EXIT_REASON_EPT_VIOLATION` is defined:

```c
/* file: module/vmx.h */
#define EXIT_REASON_EPT_VIOLATION 48
```

### EPT Misconfiguration

Another EPT-derived VM-exit is EPT Misconfiguration (`EXIT_REASON_EPT_MISCONFIG`).

An EPT Misconfiguration occurs when, in the course of translating a physical guest address, the logical processor encounters an EPT paging-structure entry that contains an unsupported value.

If you want to know more about all the reasons why EPT Misconfiguration occurs, see Intel SDM Vol 3C, Section 28.2.3.1.

Based on my experience, I encountered EPT Misconfiguration most of the time because I cleared bit 0 of the entry (indicating that data reads are not allowed) while bit 1 is set (reporting that data writes are permitted). This is an illegal combination — you cannot allow writes without allowing reads.

Also, EPT misconfigurations occur when an EPT paging-structure entry is configured with settings reserved for future functionality.

It's a fatal error. Let's just break and see what we've done wrong:

```c
/* file: module/vmx.c — inside main_vmexit_handler() */
case EXIT_REASON_EPT_MISCONFIGURATION:
{
    uint64_t guest_phys = 0;
    vmread(GUEST_PHYSICAL_ADDRESS, &guest_phys);
    printk(KERN_ERR "[*] Hyperion: EPT misconfiguration at GPA=0x%llx\n",
           guest_phys);
    BUG();
    break;
}
```

```c
/* file: module/vmx.h */
#define EXIT_REASON_EPT_MISCONFIG 49
```

### Adding EPT to VMCS

Our hypervisor starts virtualizing MMU by calling `EptLogicalProcessorInitialize`, which sets a 64-bit value called EPTP. The EPTP (Extended Page Table Pointer) structure is:

| Bits   | Field                          |
|--------|--------------------------------|
| 2:0    | Memory type (EPT paging structure cacheability) |
| 5:3    | Page walk length (value = walk depth - 1)      |
| 6      | Enable access and dirty flags                  |
| 11:7   | Reserved (must be 0)                           |
| 47:12  | PML4 address (PFN of EPT PML4 table)           |
| 63:48  | Reserved (must be 0)                           |

`EptLogicalProcessorInitialize` calls `EptAllocateAndCreateIdentityPageTable` to allocate the identity table.

For performance, we let the processor know it can cache the EPT (MemoryType = Write-Back).

We are not utilizing the 'access' and 'dirty' flag features.

As Intel mentioned, the Page Walk Length should be the count of tables we use (4) minus 1, so `PageWalkLength = 3` indicates an EPT page-walk length of 4. This is because we're using all four tables: PML4, PML3, PML2, and (after splitting) PML1.

```c
/* file: module/ept.c */
bool EptLogicalProcessorInitialize(void)
{
    VMM_EPT_PAGE_TABLE *PageTable;
    EPTP eptp;

    if (!EptCheckFeatures())
        return false;

    g_ept_state = kzalloc(sizeof(EPT_STATE), GFP_KERNEL);
    if (!g_ept_state) {
        pr_err("[*] Hyperion: failed to allocate EPT state\n");
        return false;
    }

    if (!EptBuildMtrrMap())
        return false;

    /* Allocate the identity-mapped page table */
    PageTable = EptAllocateAndCreateIdentityPageTable();
    if (!PageTable) {
        pr_err("[*] Hyperion: unable to allocate memory for EPT\n");
        return false;
    }

    g_ept_state->EptPageTable = PageTable;

    /* Set up the EPTP */
    memset(&eptp, 0, sizeof(eptp));
    eptp.fields.memory_type = MEMORY_TYPE_WRITE_BACK;  /* cacheable EPT */
    eptp.fields.dirty_access_enabled = 0;              /* no A/D flags    */
    eptp.fields.page_walk_length = 3;                  /* 4 levels - 1    */
    eptp.fields.pml4_address =
        virtual_to_physical(&PageTable->PML4[0]) / PAGE_SIZE;

    g_ept_state->EptPointer = eptp;

    return true;
}
```

Finally, we need to configure the VMCS with our EPTP:

```c
/* file: module/vmx.c — inside setup_vmcs() */
vmwrite(EPT_POINTER, g_ept_state->EptPointer.all);
```

Also, don't forget to enable the EPT feature in Secondary Processor-Based VM-Execution Controls using `CPU_BASED_CTL2_ENABLE_EPT`; otherwise, it won't work:

```c
/* file: module/vmx.c — inside setup_vmcs() */
vmwrite(SECONDARY_VM_EXEC_CONTROL,
        adjust_controls(CPU_BASED_CTL2_RDTSCP |
                        CPU_BASED_CTL2_ENABLE_EPT |
                        CPU_BASED_CTL2_ENABLE_INVPCID |
                        CPU_BASED_CTL2_ENABLE_XSAVE_XRSTORS,
                        MSR_IA32_VMX_PROCBASED_CTLS2));
```

Now we have a perfect EPT table which virtualizes MMU, and now all of the translations go through the EPT.

---

## Monitoring Page's RWX Activity

The next important topic is monitoring a page's RWX (Read/Write/Execute) activity. From the above section, you saw that we set each of Read Access, Write Access, and Execute Access to 1. But to use EPT's monitoring features, we have to set some of them to 0 so that we get EPT Violation on each of those accesses.

> **What is RWX monitoring and why is it hard?**  
> Setting an access bit to 0 causes a VM-exit every time the guest tries that kind of access to the page. This lets you *monitor* or *hook* specific pages. But this has difficulties: you're now in VMX root mode (the host context), and you can't freely call kernel APIs that might sleep or require preemption. Problems relating to memory allocation, page splitting, synchronization, and deadlock are the main limitations.

### Pre-allocating Buffers for VMX Root Mode

After executing VMLAUNCH, we shouldn't modify EPT tables from VMX non-root mode; if we do, it might (and will) cause system inconsistency.

> **Why can't we modify EPT from non-root mode?**  
> In the ideal world, no memory of the hypervisor should be visible from the virtualized OS. In our self-virtualizing hypervisor, the guest *can* see hypervisor memory because of identity EPT mapping. But that doesn't mean the guest should be allowed to modify EPT structures. More importantly, each core has its own TLB caches derived from EPT. If one core modifies the EPT while another core is using cached translations, you get inconsistency. There's also a fundamental security reason: can you modify regular page tables from user-mode? No — because page tables are in kernel memory. Similarly, EPT modifications should happen in VMX root mode.

One of the challenges is that we might need to split a 2 MB page into 4 KB pages, which requires a new PML1 table. This needs memory allocation.

We can't use `kmalloc` in VMX root mode safely. The reason is that `kmalloc` may need to sleep (for `GFP_KERNEL` allocations), and in VMX root mode we're essentially running at the highest priority with preemption and interrupts in a special state. Using sleeping allocations can deadlock.

> **Linux kernel allocation and sleeping**  
> In the Linux kernel, `kmalloc` with `GFP_KERNEL` is allowed to sleep (it can wait for memory to become available). With `GFP_ATOMIC`, it cannot sleep but may fail. In VMX root mode, we cannot sleep because we're handling a VM-exit and the processor is in a special state. So we must pre-allocate buffers from VMX non-root mode and use them in VMX root mode.

The solution is to use a previously allocated buffer from VMX non-root mode and use it in VMX root mode. This brings us the first limitation: we have to start setting hooks from VMX non-root mode because we want to pre-allocate a buffer, then pass the buffer and hook settings to VMX root mode using a VMCALL.

By the way, this is not an unsolvable limitation — you can allocate 100 pages from VMX non-root mode and use them whenever you want in VMX root mode.

We add a pre-allocated buffer pointer to our per-CPU guest state:

```c
/* file: module/hyperion.h — add to struct virtual_machine_state */
struct virtual_machine_state {
    /* ... existing fields ... */

    void *pre_allocated_buffer;  /* pre-allocated buffer for VMX root mode
                                  * EPT splitting (NULL = not allocated) */
    bool is_on_vmx_root_mode;    /* tracks whether we're in root mode */
};
```

Now, when we want to hook, we check whether the current core has a previously pre-allocated buffer or not. If it doesn't, we allocate it:

```c
/* file: module/ept.c — inside EptPageHook() */
int LogicalCoreIndex = smp_processor_id();

if (g_guest_state[LogicalCoreIndex].pre_allocated_buffer == NULL)
{
    void *PreAllocBuff = kmalloc(sizeof(VMM_EPT_DYNAMIC_SPLIT), GFP_KERNEL);
    if (!PreAllocBuff) {
        pr_err("[*] Hyperion: insufficient memory for pre-allocated buffer\n");
        return false;
    }
    memset(PreAllocBuff, 0, sizeof(VMM_EPT_DYNAMIC_SPLIT));
    g_guest_state[LogicalCoreIndex].pre_allocated_buffer = PreAllocBuff;
}
```

### Setting Hook Before VMLAUNCH

I prefer to do everything in one function so that `EptVmxRootModePageHook` can be used for both VMX root mode and non-root mode. You shouldn't directly call this function as it needs a preparing phase — instead, call `EptPageHook`.

What we have to do is call `EptVmxRootModePageHook` with a `HasLaunched` flag that determines whether we already used our EPT in VMX operation or not:

```c
/* file: module/ept.c — inside EptPageHook() */
if (EptVmxRootModePageHook(TargetFunc, HasLaunched)) {
    printk(KERN_INFO "[*] Hyperion: hook applied (VM has not launched)\n");
    return true;
}
```

### Setting Hook After VMLAUNCH

If we already used this EPT in our VMX operation, then we need to ask VMX root mode to modify the EPT table for us. In other words, we have to call `EptVmxRootModePageHook` from VMX root mode, so it requires a VMCALL.

We also have some additional things to do here: each logical core has its own set of caches relating to EPT, so we have to invalidate all the cores' EPT tables immediately. This has to be done in VMX non-root mode as we want to use kernel APIs.

```c
/* file: module/ept.c — inside EptPageHook() */
if (HasLaunched)
{
    if (AsmVmxVmcall(VMCALL_EXEC_HOOK_PAGE, TargetFunc, 0, 0) == 0)
    {
        printk(KERN_INFO "[*] Hyperion: hook applied from VMX root mode\n");

        /* Notify all cores to invalidate their EPT */
        HvNotifyAllToInvalidateEpt();
        return true;
    }
}
```

In the VMCALL handler, we just call `EptVmxRootModePageHook`:

```c
/* file: module/vmx.c — inside VmxVmcallHandler() */
case VMCALL_EXEC_HOOK_PAGE:
{
    if (EptVmxRootModePageHook(OptionalParam1, true))
        VmcallStatus = 0;
    else
        VmcallStatus = -1;
    break;
}
```

Let's get down to the invalidation part.

`HvNotifyAllToInvalidateEpt` uses `on_each_cpu` which broadcasts `HvInvalidateEptByVmcall` on all cores:

> **What is `on_each_cpu`?**  
> `on_each_cpu(func, info, wait)` is a Linux kernel function that runs `func(info)` on every online CPU. If `wait` is 1, it blocks until all CPUs have completed the function. This is the Linux equivalent of broadcasting an IPI (Inter-Processor Interrupt) to all cores and waiting for them to synchronize.

```c
/* file: module/ept.c */
void HvNotifyAllToInvalidateEpt(void)
{
    on_each_cpu(HvInvalidateEptByVmcall,
                (void *)g_ept_state->EptPointer.all, 1);
}
```

As the invalidation should be within VMX root mode (the INVEPT instruction is only valid in VMX root mode), `HvInvalidateEptByVmcall` uses VMCALL to notify VMX root mode about invalidation:

```c
/* file: module/ept.c */
static void HvInvalidateEptByVmcall(void *info)
{
    uint64_t Context = (uint64_t)info;

    if (Context == 0)
    {
        /* Invalidate all contexts */
        AsmVmxVmcall(VMCALL_INVEPT_ALL_CONTEXT, 0, 0, 0);
    }
    else
    {
        /* Invalidate a single context */
        AsmVmxVmcall(VMCALL_INVEPT_SINGLE_CONTEXT, Context, 0, 0);
    }
}
```

The VMCALL handler calls `InveptSingleContext` or `InveptAllContexts` based on the VMCALL number in VMX root mode. We'll talk about invalidation in detail later.

### Finding a Page's Entry in EPT Tables

Let's see how we can find addresses in PML1, PML2, PML3, and PML4.

#### Finding PML4, PML3, PML2 Entries

We want to find the PML2 entry. For finding PML2, first we have to find PML4 and PML3.

We used an ordinal approach to map the physical addresses, so all the physical addresses are stored in the same way. We need some definitions to find the index of the entries from tables:

```c
/* file: module/ept.h */
/* Index of the 1st paging structure (4096 byte) */
#define ADDRMASK_EPT_PML1_INDEX(var) (((var) & 0x1FF000ULL) >> 12)

/* Index of the 2nd paging structure (2MB) */
#define ADDRMASK_EPT_PML2_INDEX(var) (((var) & 0x3FE00000ULL) >> 21)

/* Index of the 3rd paging structure (1GB) */
#define ADDRMASK_EPT_PML3_INDEX(var) (((var) & 0x7FC0000000ULL) >> 30)

/* Index of the 4th paging structure (512GB) */
#define ADDRMASK_EPT_PML4_INDEX(var) (((var) & 0xFF8000000000ULL) >> 39)
```

> **How do these masks work?**  
> Each EPT level uses 9 bits of the physical address to index into its table (512 entries = 9 bits). The masks isolate those 9 bits at their respective positions:
> - PML1: bits 20:12 (4 KB page offset within a 2 MB region)
> - PML2: bits 29:21 (2 MB page offset within a 1 GB region)
> - PML3: bits 38:30 (1 GB page offset within a 512 GB region)
> - PML4: bits 47:39 (512 GB page offset)

After finding the indexes, we return the virtual address to that entry from the EPT page table:

```c
/* file: module/ept.c */
EPT_PML2_ENTRY *EptGetPml2Entry(VMM_EPT_PAGE_TABLE *EptPageTable,
                                 uint64_t PhysicalAddress)
{
    uint64_t Directory, DirectoryPointer, PML4Entry;

    Directory = ADDRMASK_EPT_PML2_INDEX(PhysicalAddress);
    DirectoryPointer = ADDRMASK_EPT_PML3_INDEX(PhysicalAddress);
    PML4Entry = ADDRMASK_EPT_PML4_INDEX(PhysicalAddress);

    /* Addresses above 512 GB are invalid */
    if (PML4Entry > 0)
        return NULL;

    return &EptPageTable->PML2[DirectoryPointer][Directory];
}
```

#### Finding PML1 Entry

For PML1, we have the same approach. First, we find the PML2 entry the same way as above. Then we check if the PML2 is split or not — if it's not split before, then we don't have PML1 and it's 3-level paging.

Finally, as we saved physical addresses contiguously, we can find the index using `ADDRMASK_EPT_PML1_INDEX` and then return the virtual address to that page entry:

```c
/* file: module/ept.c */
EPT_PML1_ENTRY *EptGetPml1Entry(VMM_EPT_PAGE_TABLE *EptPageTable,
                                 uint64_t PhysicalAddress)
{
    uint64_t Directory, DirectoryPointer, PML4Entry;
    EPT_PML2_ENTRY *PML2;
    EPT_PML1_ENTRY *PML1;
    EPT_PML2_POINTER *PML2Pointer;

    Directory = ADDRMASK_EPT_PML2_INDEX(PhysicalAddress);
    DirectoryPointer = ADDRMASK_EPT_PML3_INDEX(PhysicalAddress);
    PML4Entry = ADDRMASK_EPT_PML4_INDEX(PhysicalAddress);

    if (PML4Entry > 0)
        return NULL;

    PML2 = &EptPageTable->PML2[DirectoryPointer][Directory];

    /* Check if the page is already split (LargePage = 0 means it's a pointer) */
    if (PML2->fields.large_page)
        return NULL;

    /* Convert to PML2 pointer — these occupy the same memory location */
    PML2Pointer = (EPT_PML2_POINTER *)PML2;

    /* Translate the PML1 table's physical address to virtual */
    PML1 = (EPT_PML1_ENTRY *)physical_to_virtual(
        (void *)(uintptr_t)(PML2Pointer->fields.physical_address * PAGE_SIZE));

    if (!PML1)
        return NULL;

    /* Index into PML1 for the target address */
    PML1 = &PML1[ADDRMASK_EPT_PML1_INDEX(PhysicalAddress)];

    return PML1;
}
```

### Splitting 2 MB Pages to 4 KB Pages

As you know, in all of our hypervisor parts we used 3-level paging (PML4, PML3, PML2) and our granularity is 2 MB. Having pages with 2 MB granularity is not adequate for monitoring purposes because we might get lots of unrelated violations caused by non-relevant areas of memory.

To fix these kind of problems, we use PML1 and 4 KB granularity.

This is where we might need an additional buffer, and as we're in VMX root mode, we'll use our previously pre-allocated buffers.

First, we get the actual entry from PML2 and check if it's already split. If it was previously split, nothing to do:

```c
/* file: module/ept.c — inside EptSplitLargePage() */
EPT_PML2_ENTRY *TargetEntry;

TargetEntry = EptGetPml2Entry(EptPageTable, PhysicalAddress);
if (!TargetEntry) {
    pr_err("[*] Hyperion: an invalid physical address passed\n");
    return false;
}

/* If LargePage is 0, it's already a pointer — already split */
if (!TargetEntry->fields.large_page)
    return true;
```

If not split, we set the pre-allocated buffer pointer to NULL so that next time the pre-allocator allocates a new buffer:

```c
/* file: module/ept.c — inside EptSplitLargePage() */
g_guest_state[CoreIndex].pre_allocated_buffer = NULL;
```

Then, we fill the PML1 with an RWX template and split our 2 MB page into 4 KB chunks:

```c
/* file: module/ept.c — inside EptSplitLargePage() */
VMM_EPT_DYNAMIC_SPLIT *NewSplit;
EPT_PML1_ENTRY EntryTemplate = {0};

NewSplit = (VMM_EPT_DYNAMIC_SPLIT *)PreAllocatedBuffer;
if (!NewSplit) {
    pr_err("[*] Hyperion: failed to allocate dynamic split memory\n");
    return false;
}
memset(NewSplit, 0, sizeof(VMM_EPT_DYNAMIC_SPLIT));

/* Point back to the entry being split */
NewSplit->Entry = TargetEntry;

/* Make an RWX template */
EntryTemplate.fields.read = 1;
EntryTemplate.fields.write = 1;
EntryTemplate.fields.execute = 1;

/* Copy the template into all 512 PML1 entries */
for (int i = 0; i < VMM_EPT_PML1E_COUNT; i++)
    NewSplit->PML1[i] = EntryTemplate;

/* Set the page frame numbers for identity mapping */
for (int EntryIndex = 0; EntryIndex < VMM_EPT_PML1E_COUNT; EntryIndex++)
{
    /* Convert the 2 MB page frame number to 4 KB page entry numbers */
    NewSplit->PML1[EntryIndex].fields.physical_address =
        ((TargetEntry->fields.page_frame_number * SIZE_2_MB) / PAGE_SIZE) +
        EntryIndex;
}
```

Finally, create a new PML2 entry (with `LargePage = 0`) and replace the previous PML2 entry. Also keep track of allocated memory to de-allocate it when we want to run VMXOFF:

```c
/* file: module/ept.c — inside EptSplitLargePage() */
EPT_PML2_POINTER NewPointer = {0};
NewPointer.fields.read = 1;
NewPointer.fields.write = 1;
NewPointer.fields.execute = 1;
NewPointer.fields.physical_address =
    virtual_to_physical(&NewSplit->PML1[0]) / PAGE_SIZE;

/* Add our allocation to the linked list for later deallocation */
list_add(&NewSplit->DynamicSplitList, &EptPageTable->DynamicSplitList);

/* Replace the entry in the page table with our new split pointer */
memcpy(TargetEntry, &NewPointer, sizeof(NewPointer));
```

The full function:

```c
/* file: module/ept.c */
bool EptSplitLargePage(VMM_EPT_PAGE_TABLE *EptPageTable, void *PreAllocatedBuffer,
                       uint64_t PhysicalAddress, int CoreIndex)
{
    VMM_EPT_DYNAMIC_SPLIT *NewSplit;
    EPT_PML1_ENTRY EntryTemplate;
    EPT_PML2_ENTRY *TargetEntry;
    EPT_PML2_POINTER NewPointer;

    /* Find the PML2 entry that's currently used */
    TargetEntry = EptGetPml2Entry(EptPageTable, PhysicalAddress);
    if (!TargetEntry) {
        pr_err("[*] Hyperion: an invalid physical address passed\n");
        return false;
    }

    /* Already split? */
    if (!TargetEntry->fields.large_page)
        return true;

    /* Free previous buffer reference */
    g_guest_state[CoreIndex].pre_allocated_buffer = NULL;

    /* Use the pre-allocated buffer for the new PML1 table */
    NewSplit = (VMM_EPT_DYNAMIC_SPLIT *)PreAllocatedBuffer;
    if (!NewSplit) {
        pr_err("[*] Hyperion: failed to allocate dynamic split memory\n");
        return false;
    }
    memset(NewSplit, 0, sizeof(VMM_EPT_DYNAMIC_SPLIT));

    NewSplit->Entry = TargetEntry;

    /* RWX template for PML1 entries */
    EntryTemplate.all = 0;
    EntryTemplate.fields.read = 1;
    EntryTemplate.fields.write = 1;
    EntryTemplate.fields.execute = 1;

    for (int i = 0; i < VMM_EPT_PML1E_COUNT; i++)
        NewSplit->PML1[i] = EntryTemplate;

    /* Identity-map each 4 KB page */
    for (int EntryIndex = 0; EntryIndex < VMM_EPT_PML1E_COUNT; EntryIndex++)
    {
        NewSplit->PML1[EntryIndex].fields.physical_address =
            ((TargetEntry->fields.page_frame_number * SIZE_2_MB) / PAGE_SIZE) +
            EntryIndex;
    }

    /* Create the new PML2 pointer */
    NewPointer.all = 0;
    NewPointer.fields.read = 1;
    NewPointer.fields.write = 1;
    NewPointer.fields.execute = 1;
    NewPointer.fields.physical_address =
        virtual_to_physical(&NewSplit->PML1[0]) / PAGE_SIZE;

    /* Track for later deallocation */
    list_add(&NewSplit->DynamicSplitList, &EptPageTable->DynamicSplitList);

    /* Replace the 2 MB entry with the pointer to 512 4 KB entries */
    memcpy(TargetEntry, &NewPointer, sizeof(NewPointer));

    return true;
}
```

### Applying the Hook

`EptVmxRootModePageHook` is one of the important parts of the EPT.

First, we check to prohibit calling this function from VMX root mode when the pre-allocated buffer isn't available:

```c
/* file: module/ept.c — inside EptVmxRootModePageHook() */
int LogicalCoreIndex = smp_processor_id();

if (g_guest_state[LogicalCoreIndex].is_on_vmx_root_mode &&
    g_guest_state[LogicalCoreIndex].pre_allocated_buffer == NULL &&
    HasLaunched)
{
    return false;
}
```

> **What is `smp_processor_id()`?**  
> `smp_processor_id()` returns the ID of the CPU that is currently executing the code. This is the Linux equivalent of `KeGetCurrentProcessorNumber()` on Windows. It's needed because each CPU has its own per-CPU guest state structure, and we need to access the correct one.

Then we align the address, as the addresses in page tables are aligned:

```c
/* file: module/ept.c — inside EptVmxRootModePageHook() */
void *VirtualTarget = (void *)((uintptr_t)TargetFunc & ~(PAGE_SIZE - 1));
uint64_t PhysicalAddress = virtual_to_physical(VirtualTarget);
```

We check the granularity and split it if it's a large page:

```c
/* file: module/ept.c — inside EptVmxRootModePageHook() */
void *TargetBuffer = g_guest_state[LogicalCoreIndex].pre_allocated_buffer;

if (!EptSplitLargePage(g_ept_state->EptPageTable, TargetBuffer,
                       PhysicalAddress, LogicalCoreIndex))
{
    pr_err("[*] Hyperion: could not split page for address 0x%llx\n",
           PhysicalAddress);
    return false;
}
```

Then find the PML1 entry of the requested page (since it's already divided into 4 KB pages, PML1 is available):

```c
/* file: module/ept.c — inside EptVmxRootModePageHook() */
EPT_PML1_ENTRY *TargetPage;
EPT_PML1_ENTRY OriginalEntry;

TargetPage = EptGetPml1Entry(g_ept_state->EptPageTable, PhysicalAddress);
if (!TargetPage) {
    pr_err("[*] Hyperion: failed to get PML1 entry of target address\n");
    return false;
}

/* Save the original permissions */
OriginalEntry = *TargetPage;
```

Now we change the attributes of the PML1 entry. This is the most interesting part — for example, you can disable write access to a 4 KB page. In our case, I disable instruction execution (fetch) from the target page:

```c
/* file: module/ept.c — inside EptVmxRootModePageHook() */
/*
 * Mark the entry as no-execute. This will cause the next instruction
 * fetch from this page to cause an EPT violation.
 */
OriginalEntry.fields.read = 1;
OriginalEntry.fields.write = 1;
OriginalEntry.fields.execute = 0;

/* Apply the hook to EPT */
TargetPage->all = OriginalEntry.all;
```

If we are in VMX root mode, the TLB caches have to be invalidated:

```c
/* file: module/ept.c — inside EptVmxRootModePageHook() */
if (HasLaunched)
{
    INVEPT_DESCRIPTOR Descriptor;
    Descriptor.ept_pointer = g_ept_state->EptPointer.all;
    Descriptor.reserved = 0;
    AsmInvept(1, &Descriptor);
}
```

Done! The hook is applied.

### Handling Hooked Pages' VM-Exits

First, we align the guest physical address (since we can only find aligned physical addresses in our EPT table):

```c
/* file: module/ept.c — inside EptHandlePageHookExit() */
uint64_t PhysicalAddress = GuestPhysicalAddr & ~(PAGE_SIZE - 1);
```

Now, we find the PML1 entry relating to this physical address. We're not looking for PML2 because if we reached here, we probably already split 2 MB pages to 4 KB pages and have PML1:

```c
/* file: module/ept.c — inside EptHandlePageHookExit() */
EPT_PML1_ENTRY *TargetPage;

TargetPage = EptGetPml1Entry(g_ept_state->EptPageTable, PhysicalAddress);
if (!TargetPage) {
    pr_err("[*] Hyperion: failed to get PML1 entry for target address\n");
    return false;
}
```

Finally, we check if the violation was caused by an execute access (based on Exit Qualification) and the violated page has Execute Access set to 0. If so, we make the page's entry in PML1 executable again and invalidate the cache so that this modification takes effect.

> **Why redo the instruction?**  
> When an EPT violation occurs, the instruction that caused it did *not* execute. After we fix the EPT entry (e.g., re-enabling execute access), we need to tell our VM-exit handler to *not* skip the current instruction — we want it to execute again. This is done by not advancing GUEST_RIP. In our handler, we simply don't call `resume_to_next_instruction()` for this case.

```c
/* file: module/ept.c — inside EptHandlePageHookExit() */
/*
 * If the violation was due to trying to execute a non-executable page,
 * we make the page executable again so the instruction can execute.
 */
if (!ViolationQualification.fields.ept_executable &&
    ViolationQualification.fields.execute_access)
{
    TargetPage->fields.execute = 1;

    /* Invalidate EPT cache */
    INVEPT_DESCRIPTOR Descriptor;
    Descriptor.ept_pointer = g_ept_state->EptPointer.all;
    Descriptor.reserved = 0;
    AsmInvept(1, &Descriptor);

    /* Redo the instruction — do NOT advance RIP */
    g_guest_state[smp_processor_id()].increment_rip = false;

    printk(KERN_INFO "[*] Hyperion: set Execute Access of page "
                     "(PFN=0x%llx) to 1\n",
           TargetPage->fields.physical_address);

    return true;
}
```

We need to add the `increment_rip` field to our per-CPU state:

```c
/* file: module/hyperion.h — add to struct virtual_machine_state */
bool increment_rip;  /* if false, don't advance GUEST_RIP on VMRESUME */
```

And in our VM-exit handler, before calling `vm_resume_instruction`, we check this flag:

```c
/* file: module/vmx.c — inside main_vmexit_handler(), at the end */
/* If increment_rip is false (e.g., EPT violation handler set it),
 * don't advance RIP — the instruction needs to re-execute. */
if (g_guest_state[smp_processor_id()].increment_rip)
    resume_to_next_instruction();
else
    g_guest_state[smp_processor_id()].increment_rip = true; /* reset for next time */

return should_terminate;
```

All in all, we have the following handler:

```c
/* file: module/ept.c */
bool EptHandlePageHookExit(VMX_EXIT_QUALIFICATION_EPT_VIOLATION ViolationQualification,
                           uint64_t GuestPhysicalAddr)
{
    uint64_t PhysicalAddress;
    EPT_PML1_ENTRY *TargetPage;

    PhysicalAddress = GuestPhysicalAddr & ~(PAGE_SIZE - 1);

    if (!PhysicalAddress) {
        pr_err("[*] Hyperion: target address could not be mapped\n");
        return false;
    }

    TargetPage = EptGetPml1Entry(g_ept_state->EptPageTable, PhysicalAddress);
    if (!TargetPage) {
        pr_err("[*] Hyperion: failed to get PML1 entry for target address\n");
        return false;
    }

    /* If the violation was due to trying to execute a non-executable page */
    if (!ViolationQualification.fields.ept_executable &&
        ViolationQualification.fields.execute_access)
    {
        TargetPage->fields.execute = 1;

        INVEPT_DESCRIPTOR Descriptor;
        Descriptor.ept_pointer = g_ept_state->EptPointer.all;
        Descriptor.reserved = 0;
        AsmInvept(1, &Descriptor);

        /* Redo the instruction */
        g_guest_state[smp_processor_id()].increment_rip = false;

        printk(KERN_INFO "[*] Hyperion: set Execute Access of page "
                         "(PFN=0x%llx) to 1\n",
               TargetPage->fields.physical_address);

        return true;
    }

    pr_err("[*] Hyperion: invalid page swapping logic in hooked page\n");
    return false;
}
```

And the EPT violation entry point:

```c
/* file: module/ept.c */
bool EptHandleEptViolation(VMX_EXIT_QUALIFICATION_EPT_VIOLATION qual,
                           uint64_t GuestPhysicalAddr)
{
    if (EptHandlePageHookExit(qual, GuestPhysicalAddr))
        return true;

    pr_err("[*] Hyperion: unexpected EPT violation at GPA=0x%llx\n",
           GuestPhysicalAddr);
    return false;
}
```

---

## Invalidating Translations Derived from EPT (INVEPT)

Now that we implemented EPT, there is another problem: it's the software's responsibility to invalidate the caches. For example, we changed the Execute access attribute of a particular page — now we have to tell the CPU that we changed something and it has to invalidate its cache. Or, we get EPT Violation for Execute access of a special page and now we no longer need these EPT Violations for this page. We set the Execute Access back to 1, but we have to tell our processor that we changed something in our page table.

> **Why do we need INVEPT?**  
> Imagine we access physical address `0x1000`, and it gets translated to host physical address `0x1000` (based on 1:1 mapping). Next time we access `0x1000`, the CPU won't send the request to the memory bus — it uses the cached translation instead. This is faster. Now if we change the EPT entry (change the physical address it points to, or change the access attributes), the CPU might still use the old cached translation. INVEPT tells the processor to discard all cached EPT translations, forcing a fresh walk through the EPT tables on the next access.

There is a problem here: we have to separately tell each logical core that it needs to invalidate its EPT cache. Each core has to execute INVEPT in its own VMX root mode.

There are two types of TLB invalidation for hypervisors:

- **INVEPT** — Invalidate cached Extended Page Table (EPT) mappings to synchronize address translation with memory-resident EPT pages.
- **INVVPID** — Invalidate cached mappings based on Virtual Processor ID (VPID). We'll talk about INVVPID in detail in Part 8.

If you wouldn't perform INVEPT after changing EPT's structures, you would risk the CPU reusing old translations.

Any change to EPT structure needs INVEPT, but switching EPT (or VMCS) doesn't require INVEPT because translations are tagged with the EPTP in the cache.

We have two terms: **Single-Context** and **All-Contexts**.

```c
/* file: module/ept.h */
typedef enum {
    SINGLE_CONTEXT = 0x00000001,
    ALL_CONTEXTS   = 0x00000002
} INVEPT_TYPE;
```

We need an INVEPT descriptor:

```c
/* file: module/ept.h */
typedef struct _INVEPT_DESCRIPTOR {
    uint64_t ept_pointer;
    uint64_t reserved;
} INVEPT_DESCRIPTOR;
```

And we need an assembly function which executes the INVEPT instruction:

```c
/* file: module/exit_handler.S */
.globl AsmInvept
AsmInvept:
    invept   %rdi, (%rsi)
    jz       .Linvept_jz
    jc       .Linvept_jc
    xor      %rax, %rax
    ret
.Linvept_jz:
    mov      $1, %rax
    ret
.Linvept_jc:
    mov      $2, %rax
    ret
```

> **AT&T syntax note**  
> In AT&T assembly syntax (used by GNU assembler and Linux kernel `.S` files), the operand order is `source, destination` — the opposite of Intel syntax. So `invept %rdi, (%rsi)` means: INVEPT with type=`%rdi` (the type code) and descriptor=`(%rsi)` (a memory operand pointing to the descriptor). The error codes follow the VMX convention: ZF set → error 1 (failed with status), CF set → error 2 (failed), neither set → error 0 (success).

From the above code, `RDI` describes the type (single-context or all-contexts), and `RSI` points to the descriptor for INVEPT.

We wrap the assembly function in C:

```c
/* file: module/ept.c */
uint8_t Invept(uint32_t Type, INVEPT_DESCRIPTOR *Descriptor)
{
    if (!Descriptor) {
        INVEPT_DESCRIPTOR ZeroDescriptor = {0};
        return AsmInvept(Type, &ZeroDescriptor);
    }
    return AsmInvept(Type, Descriptor);
}
```

### Invalidating All Contexts

All-Contexts means that you invalidate all EPT-derived translations (for every VM on a particular logical core):

```c
/* file: module/ept.c */
uint8_t InveptAllContexts(void)
{
    return Invept(ALL_CONTEXTS, NULL);
}
```

> **Note:** "For every VM" means every VM for a particular logical core. Each core can have multiple VMCSs and EPT tables and switch between them. It doesn't relate to the EPT table on other cores.

### Invalidating Single Context

Single-Context means that you invalidate all EPT-derived translations based on a single EPTP (in short: for a single VM on a logical core):

```c
/* file: module/ept.c */
uint8_t InveptSingleContext(uint64_t EptPointer)
{
    INVEPT_DESCRIPTOR Descriptor;
    Descriptor.ept_pointer = EptPointer;
    Descriptor.reserved = 0;
    return Invept(SINGLE_CONTEXT, &Descriptor);
}
```

### Broadcasting INVEPT to All Logical Cores Simultaneously

Let's say you have two cores and 1 EPTP. At some point you change EPT on core one — you have to invalidate EPT on all cores at that point.

> **Why can't we call `on_each_cpu` from VMX root mode?**  
> `on_each_cpu` sends IPIs (Inter-Processor Interrupts) to all cores and waits for them to respond. But in VMX root mode, we're handling a VM-exit and we can't safely wait for other cores because: (1) other cores might be in VMX non-root mode and the IPI would cause a VM-exit on them, (2) the IPI handling code might need locks or kernel infrastructure that isn't safe in root mode, and (3) we might deadlock if the other core is waiting for us. The workaround is to return to VMX non-root mode first, then call `on_each_cpu`, and in the callback execute a VMCALL to get back to root mode and execute INVEPT.

If we want to change EPT for all cores, we can call `on_each_cpu` from regular kernel mode (not VMX root mode), and in that callback we perform a VMCALL to tell our processor to invalidate its cache in VMX root mode.

If we don't immediately invalidate EPT, we might lose some EPT Violations because each logical core will have a different memory view.

If you remember from the `EptPageHook` section, we checked whether the core is already in VMX operation. If it launched, we used VMCALL to tell the processor about modifying the EPT table from VMX root mode. Right after returning from VMCALL, we called `HvNotifyAllToInvalidateEpt` to tell all the cores about the new invalidation in their EPT caches (remember, we're in VMX non-root mode so we can use kernel APIs):

```c
/* file: module/ept.c — inside EptPageHook() */
if (HasLaunched)
{
    if (AsmVmxVmcall(VMCALL_EXEC_HOOK_PAGE, TargetFunc, 0, 0) == 0)
    {
        printk(KERN_INFO "[*] Hyperion: hook applied from VMX root mode\n");
        HvNotifyAllToInvalidateEpt();
        return true;
    }
}
```

`HvNotifyAllToInvalidateEpt` uses `on_each_cpu` which broadcasts `HvInvalidateEptByVmcall` on all logical cores and passes our current EPTP:

```c
/* file: module/ept.c */
void HvNotifyAllToInvalidateEpt(void)
{
    on_each_cpu(HvInvalidateEptByVmcall,
                (void *)(uintptr_t)g_ept_state->EptPointer.all, 1);
}
```

`HvInvalidateEptByVmcall` decides whether the caller needs an all-contexts invalidation or a single-context invalidation:

```c
/* file: module/ept.c */
static void HvInvalidateEptByVmcall(void *info)
{
    uint64_t Context = (uint64_t)info;

    if (Context == 0)
        AsmVmxVmcall(VMCALL_INVEPT_ALL_CONTEXT, 0, 0, 0);
    else
        AsmVmxVmcall(VMCALL_INVEPT_SINGLE_CONTEXT, Context, 0, 0);
}
```

Finally, the VMCALL handler calls `InveptAllContexts` or `InveptSingleContext` based on the VMCALL number in VMX root mode:

```c
/* file: module/vmx.c — inside VmxVmcallHandler() */
case VMCALL_INVEPT_SINGLE_CONTEXT:
{
    InveptSingleContext(OptionalParam1);
    VmcallStatus = 0;
    break;
}
case VMCALL_INVEPT_ALL_CONTEXT:
{
    InveptAllContexts();
    VmcallStatus = 0;
    break;
}
```

The last thing: you can't execute INVEPT in VMX non-root mode as it causes a VM-exit with `EXIT_REASON_INVEPT` (0x32) and it doesn't have any effect.

That's all for INVEPT.

---

## Fixing Previous Design Issues

The rest of this article is about improving our hypervisor and fixing some issues from the previous parts, supporting new features, and defeating deadlocks and synchronization problems.

### Supporting More Than 64 Logical Cores

Previous versions of the hypervisor had the problem of not supporting more than a certain number of cores because of how we broadcast operations to all cores. On Linux, we use `on_each_cpu` which works for any number of CPUs, so this is not an issue for us. However, we should still be careful about per-CPU state.

> **Linux vs Windows core management**  
> On Linux, `on_each_cpu(func, info, wait)` runs a function on all online CPUs and optionally waits for completion. This works regardless of the number of CPUs. The Linux kernel also provides `smp_call_function_many` for targeting specific CPUs. These APIs handle the IPI mechanics internally, so we don't need to worry about affinity masks.

### Synchronization Problem in Exiting VMX

As most of our functions are executed simultaneously across cores, we have some problems with our previously designed routines. For example, in earlier parts we used global variables `g_GuestRSP` and `g_GuestRIP` to return to the former state. Using one global variable on all cores causes errors as one core might save its RIP and RSP (core 1), then another core (core 2) overwrites the same variables. When the first core tries to restore state, it gets the state of the second core — crash!

> **What is the race condition?**  
> Imagine core 0 saves its RIP into a global variable `g_GuestRIP`. Before core 0 can restore from it, core 1 saves *its* RIP into the same variable. Now when core 0 reads `g_GuestRIP`, it gets core 1's RIP and jumps to the wrong address. This is a classic data race. The fix is to use per-CPU storage instead of a shared global.

In order to solve this problem, we have to store a per-core structure which saves the Guest RIP and Guest RSP:

```c
/* file: module/hyperion.h */
typedef struct _VMX_VMXOFF_STATE {
    bool     is_vmxoff_executed;  /* whether VMXOFF was executed on this core */
    uint64_t guest_rip;           /* RIP to return to after VMXOFF            */
    uint64_t guest_rsp;           /* RSP to return to after VMXOFF            */
} VMX_VMXOFF_STATE;
```

We add this to our per-core `virtual_machine_state`:

```c
/* file: module/hyperion.h — add to struct virtual_machine_state */
struct virtual_machine_state {
    /* ... existing fields ... */

    VMX_VMXOFF_STATE vmxoff_state;  /* per-core VMXOFF state */
};
```

We need to broadcast VMXOFF to all logical cores. This is done by `HvTerminateVmx`, which broadcasts the termination to all cores and de-allocates all EPT-related tables and pre-allocated buffers:

```c
/* file: module/vmx.c */
void HvTerminateVmx(void)
{
    /* Broadcast VMXOFF to all logical cores */
    on_each_cpu(VmxDpcBroadcastTerminate, NULL, 1);

    /* De-allocate global EPT structures */
    if (g_ept_state) {
        /* Free each dynamic split */
        struct list_head *pos, *n;
        list_for_each_safe(pos, n, &g_ept_state->EptPageTable->DynamicSplitList)
        {
            VMM_EPT_DYNAMIC_SPLIT *split;
            split = list_entry(pos, VMM_EPT_DYNAMIC_SPLIT, DynamicSplitList);
            list_del(pos);
            kfree(split);
        }

        /* Free identity page table */
        int order = get_order(sizeof(VMM_EPT_PAGE_TABLE));
        free_pages((unsigned long)g_ept_state->EptPageTable, order);

        /* Free EPT state */
        kfree(g_ept_state);
        g_ept_state = NULL;
    }

    /* Free per-CPU guest state */
    kfree(g_guest_state);
    g_guest_state = NULL;
}
```

The broadcast function terminates the guest on each core:

```c
/* file: module/vmx.c */
static void VmxDpcBroadcastTerminate(void *info)
{
    if (!VmxTerminate())
        pr_err("[*] Hyperion: error terminating VMX on core %d\n",
               smp_processor_id());
}
```

`VmxTerminate` de-allocates per-core allocated regions (VMXON region, VMCS region, VMM stack, MSR bitmap). Since we implemented our VMCALL mechanism, we can use VMCALL to request VMXOFF from VMX root mode. Each core will run VMXOFF separately:

```c
/* file: module/vmx.c */
bool VmxTerminate(void)
{
    int CurrentCoreIndex = smp_processor_id();

    printk(KERN_INFO "[*] Hyperion: terminating VMX on logical core %d\n",
           CurrentCoreIndex);

    /* Execute VMCALL to turn off VMX from VMX root mode */
    AsmVmxVmcall(VMCALL_VMXOFF, 0, 0, 0);

    /* Free per-core allocated memory */
    kfree(g_guest_state[CurrentCoreIndex].vmxon_alloc);
    kfree(g_guest_state[CurrentCoreIndex].vmcs_alloc);
    kfree(g_guest_state[CurrentCoreIndex].vmm_stack_virt);
    kfree(g_guest_state[CurrentCoreIndex].msr_bitmap_virt);

    return true;
}
```

Our VMCALL handler calls `VmxVmxoff`. Since this function is executed in VMX root mode, it's allowed to run VMXOFF. This function also saves the GuestRIP and GuestRSP into the per-core VMX_VMXOFF_STATE structure — this is where we solved the problem, as we're not using a shared global variable anymore. It also sets `is_vmxoff_executed`:

```c
/* file: module/vmx.c */
void VmxVmxoff(void)
{
    int CurrentProcessorIndex = smp_processor_id();
    uint64_t GuestRSP = 0;
    uint64_t GuestRIP = 0;
    uint64_t GuestCr3 = 0;
    uint64_t ExitInstructionLength = 0;

    /*
     * Our callback routine may have interrupted an arbitrary user process,
     * and therefore not a thread running with the kernel's page directory.
     * Therefore if we return back to the original caller after turning off
     * VMX, it will keep our current "host" CR3 value. We want to return
     * with the correct value of the "guest" CR3, so that the currently
     * executing process continues to run with its expected address space.
     */
    vmread(GUEST_CR3, &GuestCr3);
    __write_cr3(GuestCr3);

    /* Read guest RSP and RIP */
    vmread(GUEST_RIP, &GuestRIP);
    vmread(GUEST_RSP, &GuestRSP);

    /* Read instruction length and advance past the VMCALL */
    vmread(VM_EXIT_INSTRUCTION_LEN, &ExitInstructionLength);
    GuestRIP += ExitInstructionLength;

    /* Save per-core return state */
    g_guest_state[CurrentProcessorIndex].vmxoff_state.guest_rip = GuestRIP;
    g_guest_state[CurrentProcessorIndex].vmxoff_state.guest_rsp = GuestRSP;

    /* Notify the VM-exit handler that VMX is turned off */
    g_guest_state[CurrentProcessorIndex].vmxoff_state.is_vmxoff_executed = true;

    /* Execute VMXOFF */
    asm volatile("vmxoff" ::: "cc");
}
```

As we return to the VM-exit handler, we check whether we left VMX operation:

```c
/* file: module/vmx.c — inside main_vmexit_handler() */
if (g_guest_state[smp_processor_id()].vmxoff_state.is_vmxoff_executed)
{
    return true;  /* should_terminate = 1, go to VmxoffHandler */
}
```

We also define two functions which return the per-core stack pointer and RIP for the VMXOFF handler:

```c
/* file: module/vmx.c */
uint64_t HvReturnStackPointerForVmxoff(void)
{
    return g_guest_state[smp_processor_id()].vmxoff_state.guest_rsp;
}

uint64_t HvReturnInstructionPointerForVmxoff(void)
{
    return g_guest_state[smp_processor_id()].vmxoff_state.guest_rip;
}
```

Eventually, when we detect that we left VMX operation, instead of executing VMRESUME we'll run the assembly VMXOFF handler. This function calls the above two functions and places the RSP and RIP on the stack so that when we restore the general-purpose registers, we can pop RSP and return to the previous address:

```c
/* file: module/exit_handler.S */
.globl AsmVmxoffHandler
AsmVmxoffHandler:
    subq    $0x20, %rsp             /* shadow space */

    call    HvReturnStackPointerForVmxoff
    movq    %rax, 0x88(%rsp)        /* save RSP for later */

    call    HvReturnInstructionPointerForVmxoff
    movq    %rax, %rdx              /* save RIP in RDX temporarily */

    movq    0x88(%rsp), %rbx        /* load saved RSP into RBX */
    movq    %rsp, %rcx              /* save current RSP */
    movq    %rbx, %rsp              /* switch to guest stack */
    pushq   %rdx                    /* push return address onto guest stack */
    movq    %rcx, %rsp              /* restore our stack */

    subq    $0x08, %rbx             /* adjust for the push we did */
    movq    %rbx, 0x88(%rsp)        /* save updated RSP */

    addq    $0x20, %rsp             /* remove shadow space */

    /* Restore all general-purpose registers */
    popq    %rax
    popq    %rcx
    popq    %rdx
    popq    %rbx
    popq    %rbp                    /* discard extra RBP (alignment) */
    popq    %rbp
    popq    %rsi
    popq    %rdi
    popq    %r8
    popq    %r9
    popq    %r10
    popq    %r11
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15

    popq    %rsp                    /* restore guest RSP */
    ret                             /* jump back to where VMCALL was called */
```

As you can see, we no longer have the problem of using a global variable among all the cores.

### The Issues Relating to KPTI (Kernel Page Table Isolation)

> **What is KPTI?**  
> KPTI (Kernel Page Table Isolation) is the Linux mitigation for the Meltdown vulnerability (CVE-2017-5754). When enabled, the kernel maintains two sets of page tables: one for kernel mode (full address space) and one for user mode (only a small kernel stub). Switching between them involves a `MOV CR3` instruction on every kernel entry/exit. This is the Linux equivalent of KVA Shadow on Windows.

`EXIT_REASON_CR_ACCESS` is one of the reasons that might cause a VM-exit (especially if you're subject to 1-setting of CRs in your VMCS — meaning the hardware forces CR-access exiting). Hypervisors save all the general-purpose registers every time a VM-exit occurs and then restore them at the next VMRESUME.

In the previous versions of our driver, we didn't properly save the guest RSP — we saved some incorrect value instead. That's because the guest RSP is already saved in `GUEST_RSP` in the VMCS. After VMRESUME, it's loaded automatically, so the RSP on the stack is the host RSP, not the guest RSP.

After KPTI mitigation, the kernel uses `MOV CR3, RSP` as part of its page table switching. If we saved garbage instead of the correct RSP, then we'd change CR3 to an invalid value and silently crash with a TRIPLE FAULT VM-exit. It won't give you the exact error.

For fixing this issue, we add the following code to `HandleControlRegisterAccess`, so each time a CR access VM-exit occurs and the source register is RSP (register index 4), we read the correct value from `GUEST_RSP`:

```c
/* file: module/vmx.c — inside HandleControlRegisterAccess() */
/* Because the source register might be RSP and we didn't save RSP correctly
 * (because of the pushes in the assembly handler), we have to make it point
 * to GUEST_RSP from the VMCS. */
if (RegisterIndex == 4)
{
    uint64_t GuestRsp = 0;
    vmread(GUEST_RSP, &GuestRsp);
    *RegPtr = GuestRsp;
}
```

---

## Some Tips for Debugging Hypervisors

Always try to test your hypervisor on a single-core system first (you can limit the number of CPUs by adding `nr_cpus=1` to the kernel command line). If it works, then test on multi-core. When something doesn't work on multi-core but works on single-core, you know it's a synchronization problem.

Don't try to call kernel functions that might sleep in VMX root mode. In Linux, functions that allocate memory with `GFP_KERNEL` can sleep — if you call them in VMX root mode, you'll likely get a hang or crash. Use `GFP_ATOMIC` or pre-allocated buffers instead.

Use `printk` for debugging. The kernel ring buffer captures all `printk` output, and you can read it with `dmesg`. For high-frequency VM-exits, consider using `printk_ratelimited()` to avoid flooding the log.

When testing in a nested virtualization environment (e.g., KVM guest with nested VMX), make sure nested VMX is enabled:
```bash
cat /sys/module/kvm_intel/parameters/nested
# Should print Y or 1
```

---

## Let's Test It!

### How to Test

In order to test our new hypervisor, we have two scenarios.

In the first scenario, we want to test page hook (`EptPageHook`) before executing VMLAUNCH, which means that EPT is initialized, and then we want to put the hook before entering VMX:

```c
/* file: module/ept.c — example test */
/* Hook the page containing a kernel function before VMLAUNCH */
EptPageHook((void *)kfree, false);
```

The above function puts a hook on the execution of a page containing a function (in this case `kfree`).

The second scenario is testing both VMCALL and `EptPageHook` after our hypervisor is loaded and we're in VMX non-root mode:

```c
/* file: module/driver.c — inside an IOCTL handler */
/* Test VMCALL */
if (AsmVmxVmcall(VMCALL_TEST, 0x22, 0x333, 0x4444) == 0)
{
    /* Test hook after VMX is launched */
    EptPageHook((void *)kfree, true);
    return 0;
}
return -1;
```

As you can see, it first tests the VMCALL using `VMCALL_TEST` and then puts a hook on a function (in this case `kfree`).

### Demo

First, we load our hypervisor module:

```bash
sudo insmod module/hyperion.ko
```

You should see in `dmesg`:

```
Hyperion: module loading
Hyperion: device created at /dev/hyperion
```

Then we run the user program to trigger VMX initialization:

```bash
sudo ./user/hyperion-user
```

Output:
```
[*] CPU Vendor: GenuineIntel
[*] VMX Operation is supported by your processor.
[*] VMX initialized successfully.
[*] Device opened successfully. Press Enter to exit...
```

In `dmesg`, you should see:
- VMX initialization on each CPU
- VMLAUNCH succeeded
- VM-exits being handled (CPUID, CR_ACCESS, MSR, RDTSC, etc.)

For the EPT hook test, after the hook is applied, you'll see EPT Violation exits when the hooked function is called:

```
[*] Hyperion: set Execute Access of page (PFN=0x...) to 1
```

This means the hook is working — the CPU tried to execute the hooked page, got an EPT Violation, and our handler temporarily re-enabled execute access to let the instruction through.

---

## Conclusion

EPT is the most important feature that can be used by researchers, security programs, and analysts, as it gives a unique ability to monitor the operating system and user-mode applications transparently. In the next part, we'll use EPT to implement hidden hook mechanisms. We'll also improve our hypervisor by using event injection, a mechanism to talk from VMX root mode to VMX non-root mode, and Virtual Processor Identifier (VPID).
