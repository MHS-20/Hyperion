#include "ept.h"
#include "hyperion.h"
#include "vmx.h"
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/moduleparam.h>

static bool g_enable_ept;
module_param_named(enable_ept, g_enable_ept, bool, 0644);
MODULE_PARM_DESC(enable_ept, "Enable Extended Page Tables (default: false)");

#ifndef MSR_IA32_MTRR_CAPABILITIES
#define MSR_IA32_MTRR_CAPABILITIES 0xFE
#endif
#ifndef MSR_IA32_MTRR_PHYSBASE0
#define MSR_IA32_MTRR_PHYSBASE0 0x200
#endif
#ifndef MSR_IA32_MTRR_PHYSMASK0
#define MSR_IA32_MTRR_PHYSMASK0 0x201
#endif
#ifndef MSR_IA32_MTRR_DEF_TYPE
#define MSR_IA32_MTRR_DEF_TYPE 0x2FF
#endif

static bool EptCheckFeatures(void);
static bool EptBuildMtrrMap(void);
static void EptSetupPML2Entry(EPT_PML2_ENTRY *NewEntry, uint64_t PageFrameNumber);
static VMM_EPT_PAGE_TABLE *EptAllocateAndCreateIdentityPageTable(void);

static MTRR_RANGE_DESCRIPTOR g_mtrr_ranges[MAX_MTRR_RANGES];
static uint32_t g_mtrr_range_count = 0;
EPT_STATE *g_ept_state = NULL;

static void EptSetupPML2Entry(EPT_PML2_ENTRY *NewEntry,
                               uint64_t PageFrameNumber) {
  uint64_t AddressOfPage = PageFrameNumber * SIZE_2_MB;
  uint8_t TargetMemoryType = MEMORY_TYPE_WRITE_BACK;

  for (uint32_t i = 0; i < g_mtrr_range_count; i++) {
    if (AddressOfPage <= g_mtrr_ranges[i].PhysicalEndAddress) {
      if ((AddressOfPage + SIZE_2_MB - 1) >=
          g_mtrr_ranges[i].PhysicalBaseAddress) {
        TargetMemoryType = g_mtrr_ranges[i].MemoryType;
        if (TargetMemoryType == MEMORY_TYPE_UNCACHEABLE)
          break;
      }
    }
  }

  /* For 2MB large pages, memory type is at bits 5:3 and ignore_pat
   * is at bit 6, both within the reserved1 field.  Bit 7 must be 1. */
  NewEntry->fields.page_frame_number = PageFrameNumber;
  NewEntry->fields.memory_type = 0;  /* not used for large pages */
  NewEntry->fields.ignore_pat = 0;   /* not used for large pages */
  NewEntry->fields.reserved1 =
      (1 << 4) | TargetMemoryType; /* bit 7=1, bits 5:3=memory_type */
}

static VMM_EPT_PAGE_TABLE *EptAllocateAndCreateIdentityPageTable(void) {
  VMM_EPT_PAGE_TABLE *PageTable;
  int order;

  order = get_order(sizeof(VMM_EPT_PAGE_TABLE));
  PageTable = (VMM_EPT_PAGE_TABLE *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
                                                     order);
  if (!PageTable) {
    pr_err("[*] Hyperion: failed to allocate EPT page table\n");
    return NULL;
  }

  INIT_LIST_HEAD(&PageTable->DynamicSplitList);

  /* PML4[0] -> PML3 (covers 512 GB) */
  EPT_PML4E pml4e = {0};
  pml4e.fields.read = 1;
  pml4e.fields.write = 1;
  pml4e.fields.execute = 1;
  pml4e.fields.physical_address =
      virtual_to_physical(&PageTable->PML3[0]) / PAGE_SIZE;
  PageTable->PML4[0] = pml4e;

  /* PML3[i] -> PML2[i] (each covers 1 GB) */
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

  /* PML2 — 2 MB large pages with MTRR-based memory types */
  EPT_PML2_ENTRY pml2_template = {0};
  pml2_template.fields.read = 1;
  pml2_template.fields.write = 1;
  pml2_template.fields.execute = 1;
  pml2_template.fields.large_page = 0; /* for 2MB pages, bit 7=1 (via reserved1) */
  pml2_template.fields.reserved1 = 0x10; /* bit 7=1, memory_type bits 5:3=0 (UC) */

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

bool EptLogicalProcessorInitialize(void) {
  VMM_EPT_PAGE_TABLE *PageTable;
  EPTP eptp;

  if (!EptCheckFeatures())
    return false;

  if (g_ept_state == NULL) {
    g_ept_state = kzalloc(sizeof(EPT_STATE), GFP_KERNEL);
    if (!g_ept_state) {
      pr_err("[*] Hyperion: failed to allocate EPT state\n");
      return false;
    }

    if (!EptBuildMtrrMap()) {
      kfree(g_ept_state);
      g_ept_state = NULL;
      return false;
    }

    for (uint32_t i = 0; i < g_mtrr_range_count; i++)
      g_ept_state->MemoryRanges[g_ept_state->NumberOfEnabledMemoryRanges++] =
          g_mtrr_ranges[i];

    PageTable = EptAllocateAndCreateIdentityPageTable();
    if (!PageTable) {
      pr_err("[*] Hyperion: unable to allocate memory for EPT\n");
      kfree(g_ept_state);
      g_ept_state = NULL;
      return false;
    }

    g_ept_state->EptPageTable = PageTable;

    memset(&eptp, 0, sizeof(eptp));
    eptp.fields.memory_type = MEMORY_TYPE_WRITE_BACK;
    eptp.fields.dirty_access_enabled = 0;
    eptp.fields.page_walk_length = 3;
    eptp.fields.pml4_address =
        virtual_to_physical(&PageTable->PML4[0]) / PAGE_SIZE;

    g_ept_state->EptPointer = eptp;
  }

  pr_info("[*] Hyperion: EPT pointer allocated at 0x%llx\n",
          g_ept_state->EptPointer.all);
  return true;
}

uint64_t initialize_eptp(void) {
  if (!g_enable_ept)
    return 0;
  if (!EptLogicalProcessorInitialize())
    return 0;
  return g_ept_state->EptPointer.all;
}

/* --- EPT page hook infrastructure --- */

EPT_PML2_ENTRY *EptGetPml2Entry(VMM_EPT_PAGE_TABLE *EptPageTable,
                                 uint64_t PhysicalAddress) {
  uint64_t Directory = ADDRMASK_EPT_PML2_INDEX(PhysicalAddress);
  uint64_t DirectoryPointer = ADDRMASK_EPT_PML3_INDEX(PhysicalAddress);
  uint64_t PML4Entry = ADDRMASK_EPT_PML4_INDEX(PhysicalAddress);

  if (PML4Entry > 0)
    return NULL;

  return &EptPageTable->PML2[DirectoryPointer][Directory];
}

EPT_PML1_ENTRY *EptGetPml1Entry(VMM_EPT_PAGE_TABLE *EptPageTable,
                                 uint64_t PhysicalAddress) {
  EPT_PML2_ENTRY *PML2;
  EPT_PML1_ENTRY *PML1;
  EPT_PML2_POINTER *PML2Pointer;
  uint64_t Directory = ADDRMASK_EPT_PML2_INDEX(PhysicalAddress);
  uint64_t DirectoryPointer = ADDRMASK_EPT_PML3_INDEX(PhysicalAddress);
  uint64_t PML4Entry = ADDRMASK_EPT_PML4_INDEX(PhysicalAddress);

  if (PML4Entry > 0)
    return NULL;

  PML2 = &EptPageTable->PML2[DirectoryPointer][Directory];

  if (PML2->all & (1ULL << 7))
    return NULL;

  PML2Pointer = (EPT_PML2_POINTER *)PML2;
  PML1 = (EPT_PML1_ENTRY *)physical_to_virtual(
      (uint64_t)(PML2Pointer->fields.physical_address * PAGE_SIZE));

  if (!PML1)
    return NULL;

  return &PML1[ADDRMASK_EPT_PML1_INDEX(PhysicalAddress)];
}

bool EptSplitLargePage(VMM_EPT_PAGE_TABLE *EptPageTable, void *PreAllocatedBuffer,
                       uint64_t PhysicalAddress, int CoreIndex) {
  VMM_EPT_DYNAMIC_SPLIT *NewSplit;
  EPT_PML1_ENTRY EntryTemplate;
  EPT_PML2_ENTRY *TargetEntry;
  EPT_PML2_POINTER NewPointer;

  TargetEntry = EptGetPml2Entry(EptPageTable, PhysicalAddress);
  if (!TargetEntry) {
    pr_err("[*] Hyperion: invalid physical address for page split\n");
    return false;
  }

  if (!(TargetEntry->all & (1ULL << 7)))
    return true;

  g_guest_state[CoreIndex].pre_allocated_buffer = NULL;

  NewSplit = (VMM_EPT_DYNAMIC_SPLIT *)PreAllocatedBuffer;
  if (!NewSplit) {
    pr_err("[*] Hyperion: failed to allocate dynamic split memory\n");
    return false;
  }
  memset(NewSplit, 0, sizeof(VMM_EPT_DYNAMIC_SPLIT));

  NewSplit->Entry = TargetEntry;

  EntryTemplate.all = 0;
  EntryTemplate.fields.read = 1;
  EntryTemplate.fields.write = 1;
  EntryTemplate.fields.execute = 1;

  for (int i = 0; i < VMM_EPT_PML1E_COUNT; i++)
    NewSplit->PML1[i] = EntryTemplate;

  for (int EntryIndex = 0; EntryIndex < VMM_EPT_PML1E_COUNT; EntryIndex++) {
    NewSplit->PML1[EntryIndex].fields.physical_address =
        ((TargetEntry->fields.page_frame_number * SIZE_2_MB) / PAGE_SIZE) +
        EntryIndex;
  }

  NewPointer.all = 0;
  NewPointer.fields.read = 1;
  NewPointer.fields.write = 1;
  NewPointer.fields.execute = 1;
  NewPointer.fields.physical_address =
      virtual_to_physical(&NewSplit->PML1[0]) / PAGE_SIZE;

  list_add(&NewSplit->DynamicSplitList, &EptPageTable->DynamicSplitList);
  memcpy(TargetEntry, &NewPointer, sizeof(NewPointer));

  return true;
}

bool EptVmxRootModePageHook(void *TargetFunc, bool HasLaunched) {
  int LogicalCoreIndex = smp_processor_id();
  void *VirtualTarget = (void *)((uintptr_t)TargetFunc & ~(PAGE_SIZE - 1));
  uint64_t PhysicalAddress;
  EPT_PML1_ENTRY *TargetPage;
  EPT_PML1_ENTRY OriginalEntry;
  void *TargetBuffer;

  PhysicalAddress = virtual_to_physical(VirtualTarget);
  TargetBuffer = g_guest_state[LogicalCoreIndex].pre_allocated_buffer;

  if (!EptSplitLargePage(g_ept_state->EptPageTable, TargetBuffer,
                         PhysicalAddress, LogicalCoreIndex)) {
    pr_err("[*] Hyperion: could not split page for address 0x%llx\n",
           PhysicalAddress);
    return false;
  }

  TargetPage = EptGetPml1Entry(g_ept_state->EptPageTable, PhysicalAddress);
  if (!TargetPage) {
    pr_err("[*] Hyperion: failed to get PML1 entry of target address\n");
    return false;
  }

  OriginalEntry = *TargetPage;
  OriginalEntry.fields.read = 1;
  OriginalEntry.fields.write = 1;
  OriginalEntry.fields.execute = 0;
  TargetPage->all = OriginalEntry.all;

  printk(KERN_INFO "[*] Hyperion: hook applied at phys=0x%llx "
                   "(execute disabled)\n",
         PhysicalAddress);

  return true;
}

bool EptPageHook(void *TargetFunc, bool HasLaunched) {
  int LogicalCoreIndex = smp_processor_id();

  if (g_guest_state[LogicalCoreIndex].pre_allocated_buffer == NULL) {
    void *PreAllocBuff = kmalloc(sizeof(VMM_EPT_DYNAMIC_SPLIT), GFP_KERNEL);
    if (!PreAllocBuff) {
      pr_err("[*] Hyperion: insufficient memory for pre-allocated buffer\n");
      return false;
    }
    memset(PreAllocBuff, 0, sizeof(VMM_EPT_DYNAMIC_SPLIT));
    g_guest_state[LogicalCoreIndex].pre_allocated_buffer = PreAllocBuff;
  }

  if (HasLaunched) {
    if (AsmVmxVmcall(VMCALL_EXEC_HOOK_PAGE, (uint64_t)TargetFunc, 0, 0) == 0) {
      printk(KERN_INFO "[*] Hyperion: hook applied from VMX root mode\n");
      return true;
    }
    return false;
  }

  return EptVmxRootModePageHook(TargetFunc, HasLaunched);
}

bool EptHandlePageHookExit(VMX_EXIT_QUALIFICATION_EPT_VIOLATION ViolationQualification,
                           uint64_t GuestPhysicalAddr) {
  uint64_t PhysicalAddress = GuestPhysicalAddr & ~(PAGE_SIZE - 1);
  EPT_PML2_ENTRY *PML2Entry;
  EPT_PML1_ENTRY *TargetPage;

  if (!PhysicalAddress) {
    pr_err("[*] Hyperion: target address could not be mapped\n");
    return false;
  }

  /* Try PML1 first (split page), fall back to PML2 (large page) */
  TargetPage = EptGetPml1Entry(g_ept_state->EptPageTable, PhysicalAddress);
  if (!TargetPage) {
    PML2Entry = EptGetPml2Entry(g_ept_state->EptPageTable, PhysicalAddress);
    if (!PML2Entry || !(PML2Entry->all & (1ULL << 7))) {
      pr_err("[*] Hyperion: no valid EPT entry for GPA 0x%llx\n",
             GuestPhysicalAddr);
      return false;
    }
    /* 2MB large page — just log and skip */
    printk(KERN_WARNING "[*] Hyperion: EPT violation on 2MB page at "
                        "GPA=0x%llx (PFN=%llu)\n",
           GuestPhysicalAddr, PML2Entry->fields.page_frame_number);
    return true;
  }

  if (!ViolationQualification.fields.ept_executable &&
      ViolationQualification.fields.execute_access) {
    TargetPage->fields.execute = 1;

    /* Redo the instruction */
    g_guest_state[smp_processor_id()].increment_rip = false;

    printk(KERN_INFO "[*] Hyperion: set Execute Access of page "
                     "(PFN=0x%llx) to 1\n",
           TargetPage->fields.physical_address);

    return true;
  }

  return false;
}

bool EptHandleEptViolation(uint64_t ExitQualification,
                           uint64_t GuestPhysicalAddr) {
  VMX_EXIT_QUALIFICATION_EPT_VIOLATION qual;
  qual.all = ExitQualification;

  if (EptHandleHookedPage(GuestPhysicalAddr &
                          ~(PAGE_SIZE - 1),
                          0,
                          !qual.fields.ept_readable &&
                          qual.fields.read_access,
                          !qual.fields.ept_writable &&
                          qual.fields.write_access,
                          !qual.fields.ept_executable &&
                          qual.fields.execute_access))
    return true;

  if (EptHandlePageHookExit(qual, GuestPhysicalAddr))
    return true;

  return false;
}

static void EptHookWriteAbsoluteJump(void *Destination, uint64_t TargetAddress) {
  uint8_t *Buffer = (uint8_t *)Destination;

  Buffer[0] = 0x49;
  Buffer[1] = 0xBF;
  memcpy(&Buffer[2], &TargetAddress, sizeof(uint64_t));

  Buffer[10] = 0x41;
  Buffer[11] = 0x57;

  Buffer[12] = 0xC3;
}

static void EptSetPML1AndInvalidateTLB(EPT_PML1_ENTRY *Entry,
                                       EPT_PML1_ENTRY NewValue) {
  Entry->all = NewValue.all;
  InveptSingleContext(g_ept_state->EptPointer.all);
}

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

  HookDetail = kmalloc(sizeof(EPT_HOOKED_PAGE_DETAIL), GFP_KERNEL);
  if (!HookDetail) {
    pr_err("[*] Hyperion: failed to allocate hook detail\n");
    return false;
  }
  memset(HookDetail, 0, sizeof(EPT_HOOKED_PAGE_DETAIL));

  void *PreAllocBuffer = g_guest_state[CurrentCoreIndex].pre_allocated_buffer;
  if (!EptSplitLargePage(g_ept_state->EptPageTable, PreAllocBuffer,
                         PhysicalAddress, CurrentCoreIndex)) {
    pr_err("[*] Hyperion: failed to split page for hook\n");
    kfree(HookDetail);
    return false;
  }

  TargetEntry = EptGetPml1Entry(g_ept_state->EptPageTable, PhysicalAddress);
  if (!TargetEntry) {
    pr_err("[*] Hyperion: failed to get PML1 entry\n");
    kfree(HookDetail);
    return false;
  }

  OriginalEntry = *TargetEntry;

  if (HookExecute) {
    FakePage = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!FakePage) {
      pr_err("[*] Hyperion: failed to allocate fake page\n");
      kfree(HookDetail);
      return false;
    }

    phys_addr_t OrigPhys = TargetEntry->fields.physical_address * PAGE_SIZE;
    void *OrigVirt = physical_to_virtual(OrigPhys);
    memcpy(FakePage, OrigVirt, PAGE_SIZE);

    uint64_t OffsetInPage = (uint64_t)TargetFunction - (uint64_t)VirtualTarget;
    EptHookWriteAbsoluteJump((uint8_t *)FakePage + OffsetInPage,
                              (uint64_t)HookFunction);

    EPT_PML1_ENTRY FakeEntry = OriginalEntry;
    FakeEntry.fields.physical_address =
        virtual_to_physical(FakePage) / PAGE_SIZE;
    FakeEntry.fields.read = 0;
    FakeEntry.fields.write = 0;
    FakeEntry.fields.execute = 1;

    HookDetail->FakePagePhysAddress = virtual_to_physical(FakePage);

    EptSetPML1AndInvalidateTLB(TargetEntry, FakeEntry);

    if (OrigFunction)
      *OrigFunction = TargetFunction;
  } else {
    EPT_PML1_ENTRY HookEntry = OriginalEntry;
    HookEntry.fields.read = HookRead ? 0 : 1;
    HookEntry.fields.write = HookWrite ? 0 : 1;

    HookDetail->HookedEntry = HookEntry;
    EptSetPML1AndInvalidateTLB(TargetEntry, HookEntry);
  }

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

bool EptHandleHookedPage(uint64_t PhysicalAddress, uint64_t GuestRip,
                         bool IsReadViolation, bool IsWriteViolation,
                         bool IsExecuteViolation) {
  EPT_HOOKED_PAGE_DETAIL *HookDetail;
  EPT_PML1_ENTRY *TargetEntry;

  list_for_each_entry(HookDetail, &g_HookedPagesList, HookedPagesList) {
    if (HookDetail->PhysicalAddress != PhysicalAddress)
      continue;

    TargetEntry = EptGetPml1Entry(g_ept_state->EptPageTable,
                                   PhysicalAddress);
    if (!TargetEntry)
      continue;

    if (HookDetail->HookedForExecute) {
      if (IsReadViolation || IsWriteViolation) {
        EptSetPML1AndInvalidateTLB(TargetEntry,
                                    HookDetail->OriginalEntry);

        printk(KERN_INFO "[*] Hyperion: hidden hook trigger — "
                         "RIP=0x%llx phys=0x%llx %s\n",
               GuestRip, PhysicalAddress,
               IsReadViolation ? "READ" : "WRITE");

        return true;
      }
      if (IsExecuteViolation) {
        EPT_PML1_ENTRY TempEntry = HookDetail->HookedEntry;
        TempEntry.fields.execute = 1;
        EptSetPML1AndInvalidateTLB(TargetEntry, TempEntry);

        g_guest_state[smp_processor_id()].increment_rip = false;
        return true;
      }
    } else {
      EptSetPML1AndInvalidateTLB(TargetEntry,
                                  HookDetail->OriginalEntry);

      printk(KERN_INFO "[*] Hyperion: hidden hook trigger — "
                       "RIP=0x%llx phys=0x%llx %s%s\n",
             GuestRip, PhysicalAddress,
             IsReadViolation ? "READ " : "",
             IsWriteViolation ? "WRITE" : "");

      HvSetMonitorTrapFlag(true);

      g_guest_state[smp_processor_id()].increment_rip = false;
      return true;
    }
  }

  return false;
}

void EptHandleMonitorTrapFlag(void) {
  EPT_HOOKED_PAGE_DETAIL *HookDetail;

  list_for_each_entry(HookDetail, &g_HookedPagesList, HookedPagesList) {
    EPT_PML1_ENTRY *TargetEntry;

    TargetEntry = EptGetPml1Entry(g_ept_state->EptPageTable,
                                   HookDetail->PhysicalAddress);
    if (!TargetEntry)
      continue;

    if (!HookDetail->HookedForExecute) {
      EptSetPML1AndInvalidateTLB(TargetEntry,
                                  HookDetail->HookedEntry);
    }
  }
}

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
        EptSetPML1AndInvalidateTLB(TargetEntry,
                                    HookDetail->OriginalEntry);
      }

      list_del(&HookDetail->HookedPagesList);

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

/* --- INVEPT and cache invalidation --- */

static uint8_t AsmInvept(uint32_t Type, INVEPT_DESCRIPTOR *Descriptor) {
  struct {
    uint64_t ept_pointer;
    uint64_t reserved;
  } operand = {0};
  uint8_t result = 0;

  if (Descriptor) {
    operand.ept_pointer = Descriptor->ept_pointer;
    operand.reserved = Descriptor->reserved;
  }

  asm volatile(".intel_syntax noprefix\n\t"
               "invept %[type], oword ptr [%[desc]]\n\t"
               "setna %[result]\n\t"
               ".att_syntax prefix\n\t"
               : [result] "=q"(result)
               : [type] "r"((uint64_t)Type),
                 [desc] "r"(&operand)
               : "cc", "memory");

  return result;
}

uint8_t Invept(uint32_t Type, INVEPT_DESCRIPTOR *Descriptor) {
  if (!Descriptor) {
    INVEPT_DESCRIPTOR ZeroDescriptor = {0};
    return AsmInvept(Type, &ZeroDescriptor);
  }
  return AsmInvept(Type, Descriptor);
}

uint8_t InveptAllContexts(void) {
  return Invept(INVEPT_ALL_CONTEXTS, NULL);
}

uint8_t InveptSingleContext(uint64_t EptPointer) {
  INVEPT_DESCRIPTOR Descriptor;
  Descriptor.ept_pointer = EptPointer;
  Descriptor.reserved = 0;
  return Invept(INVEPT_SINGLE_CONTEXT, &Descriptor);
}

static void HvInvalidateEptByVmcall(void *info) {
  uint64_t Context = (uint64_t)info;
  if (Context == 0)
    AsmVmxVmcall(VMCALL_INVEPT_ALL_CONTEXT, 0, 0, 0);
  else
    AsmVmxVmcall(VMCALL_INVEPT_SINGLE_CONTEXT, Context, 0, 0);
}

void HvNotifyAllToInvalidateEpt(void) {
  on_each_cpu(HvInvalidateEptByVmcall,
              (void *)(uintptr_t)g_ept_state->EptPointer.all, 1);
}

static bool EptBuildMtrrMap(void) {
  IA32_MTRR_CAPABILITIES_REGISTER MTRRCap;
  IA32_MTRR_PHYSBASE_REGISTER CurrentPhysBase;
  IA32_MTRR_PHYSMASK_REGISTER CurrentPhysMask;
  MTRR_RANGE_DESCRIPTOR *Descriptor;
  uint32_t CurrentRegister;
  int NumberOfBitsInMask;

  rdmsrl(MSR_IA32_MTRR_CAPABILITIES, MTRRCap.all);

  for (CurrentRegister = 0;
       CurrentRegister < MTRRCap.fields.variable_range_count;
       CurrentRegister++) {
    rdmsrl(MSR_IA32_MTRR_PHYSBASE0 + (CurrentRegister * 2), CurrentPhysBase.all);
    rdmsrl(MSR_IA32_MTRR_PHYSMASK0 + (CurrentRegister * 2), CurrentPhysMask.all);

    if (CurrentPhysMask.fields.valid) {
      Descriptor = &g_mtrr_ranges[g_mtrr_range_count++];

      Descriptor->PhysicalBaseAddress =
          CurrentPhysBase.fields.page_frame_number * PAGE_SIZE;

      uint64_t mask_aligned =
          CurrentPhysMask.fields.page_frame_number * PAGE_SIZE;
      NumberOfBitsInMask = __builtin_ctzll(mask_aligned);

      Descriptor->PhysicalEndAddress =
          Descriptor->PhysicalBaseAddress +
          ((1ULL << NumberOfBitsInMask) - 1ULL);

      Descriptor->MemoryType = (uint8_t)CurrentPhysBase.fields.type;

      if (Descriptor->MemoryType == MEMORY_TYPE_WRITE_BACK) {
        g_mtrr_range_count--;
      }

      printk(KERN_INFO "[*] Hyperion: MTRR Range: Base=0x%llx "
                       "End=0x%llx Type=0x%x\n",
             Descriptor->PhysicalBaseAddress, Descriptor->PhysicalEndAddress,
             Descriptor->MemoryType);
    }
  }

  printk(KERN_INFO "[*] Hyperion: Total MTRR Ranges Committed: %d\n",
         g_mtrr_range_count);
  return true;
}

static bool EptCheckFeatures(void) {
  IA32_MTRR_DEF_TYPE_REGISTER MTRRDefType;
  uint32_t eax, ebx, ecx, edx;

  rdmsrl(MSR_IA32_MTRR_DEF_TYPE, MTRRDefType.all);

  if (!MTRRDefType.fields.mtrr_enable) {
    pr_err("[*] Hyperion: MTRR dynamic ranges not supported\n");
    return false;
  }

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
