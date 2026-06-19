#include "ept.h"
#include "hyperion.h"
#include <linux/mm.h>
#include <linux/slab.h>

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

  NewEntry->fields.page_frame_number = PageFrameNumber;

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

  NewEntry->fields.memory_type = TargetMemoryType;
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
  if (!EptLogicalProcessorInitialize())
    return 0;
  return g_ept_state->EptPointer.all;
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
