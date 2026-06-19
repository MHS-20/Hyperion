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

static MTRR_RANGE_DESCRIPTOR g_mtrr_ranges[MAX_MTRR_RANGES];
static uint32_t g_mtrr_range_count = 0;

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

uint64_t initialize_eptp(void) {
  EPTP *eptp;
  EPT_PML4E *ept_pml4;
  EPT_PDPTE *ept_pdpt;
  EPT_PDE *ept_pd;
  EPT_PTE *ept_pt;
  void *guest_memory;

  const int pages_to_allocate = 10;

  eptp = kzalloc(sizeof(EPTP), GFP_KERNEL);
  if (!eptp) {
    pr_err("[*] Hyperion: failed to allocate EPTP\n");
    return 0;
  }

  /* Allocate EPT PML4 table (512 × 8 bytes = 4096 bytes) */
  ept_pml4 = kzalloc(PAGE_SIZE, GFP_KERNEL);
  if (!ept_pml4) {
    pr_err("[*] Hyperion: failed to allocate EPT PML4\n");
    kfree(eptp);
    return 0;
  }

  /* Allocate EPT Page-Directory-Pointer Table */
  ept_pdpt = kzalloc(PAGE_SIZE, GFP_KERNEL);
  if (!ept_pdpt) {
    pr_err("[*] Hyperion: failed to allocate EPT PDPT\n");
    kfree(ept_pml4);
    kfree(eptp);
    return 0;
  }

  /* Allocate EPT Page Directory */
  ept_pd = kzalloc(PAGE_SIZE, GFP_KERNEL);
  if (!ept_pd) {
    pr_err("[*] Hyperion: failed to allocate EPT PD\n");
    kfree(ept_pdpt);
    kfree(ept_pml4);
    kfree(eptp);
    return 0;
  }

  /* Allocate EPT Page Table */
  ept_pt = kzalloc(PAGE_SIZE, GFP_KERNEL);
  if (!ept_pt) {
    pr_err("[*] Hyperion: failed to allocate EPT PT\n");
    kfree(ept_pd);
    kfree(ept_pdpt);
    kfree(ept_pml4);
    kfree(eptp);
    return 0;
  }

  /*
   * Allocate contiguous guest memory.
   * We need at minimum 2 pages (one for code, one for stack), but we
   * allocate 10 to give the guest room to build its own internal page
   * tables and data structures.
   */
  guest_memory = kzalloc(pages_to_allocate * PAGE_SIZE, GFP_KERNEL);
  if (!guest_memory) {
    pr_err("[*] Hyperion: failed to allocate guest memory\n");
    kfree(ept_pt);
    kfree(ept_pd);
    kfree(ept_pdpt);
    kfree(ept_pml4);
    kfree(eptp);
    return 0;
  }

  for (int i = 0; i < pages_to_allocate; i++) {
    ept_pt[i].fields.accessed_flag = 0;
    ept_pt[i].fields.dirty_flag = 0;
    ept_pt[i].fields.ept_memory_type = 6; /* Write-Back */
    ept_pt[i].fields.execute = 1;
    ept_pt[i].fields.execute_for_usermode = 0;
    ept_pt[i].fields.ignore_pat = 0;
    ept_pt[i].fields.physical_address =
        virtual_to_physical((uint8_t *)guest_memory + (i * PAGE_SIZE)) /
        PAGE_SIZE;
    ept_pt[i].fields.read = 1;
    ept_pt[i].fields.suppress_ve = 0;
    ept_pt[i].fields.write = 1;
  }

  /* Set up PDE — points to the base of the EPT PT */
  ept_pd->fields.accessed = 0;
  ept_pd->fields.execute = 1;
  ept_pd->fields.execute_for_usermode = 0;
  ept_pd->fields.ignored1 = 0;
  ept_pd->fields.ignored2 = 0;
  ept_pd->fields.ignored3 = 0;
  ept_pd->fields.physical_address = virtual_to_physical(ept_pt) / PAGE_SIZE;
  ept_pd->fields.read = 1;
  ept_pd->fields.reserved1 = 0;
  ept_pd->fields.reserved2 = 0;
  ept_pd->fields.write = 1;

  /* Set up PDPTE — points to the base of the EPT PD */
  ept_pdpt->fields.accessed = 0;
  ept_pdpt->fields.execute = 1;
  ept_pdpt->fields.execute_for_usermode = 0;
  ept_pdpt->fields.ignored1 = 0;
  ept_pdpt->fields.ignored2 = 0;
  ept_pdpt->fields.ignored3 = 0;
  ept_pdpt->fields.physical_address = virtual_to_physical(ept_pd) / PAGE_SIZE;
  ept_pdpt->fields.read = 1;
  ept_pdpt->fields.reserved1 = 0;
  ept_pdpt->fields.reserved2 = 0;
  ept_pdpt->fields.write = 1;

  /* Set up PML4E — points to the base of the EPT PDPT */
  ept_pml4->fields.accessed = 0;
  ept_pml4->fields.execute = 1;
  ept_pml4->fields.execute_for_usermode = 0;
  ept_pml4->fields.ignored1 = 0;
  ept_pml4->fields.ignored2 = 0;
  ept_pml4->fields.ignored3 = 0;
  ept_pml4->fields.physical_address = virtual_to_physical(ept_pdpt) / PAGE_SIZE;
  ept_pml4->fields.read = 1;
  ept_pml4->fields.reserved1 = 0;
  ept_pml4->fields.reserved2 = 0;
  ept_pml4->fields.write = 1;

  /*
   * Set up the EPTP.
   * memory_type = 6 (Write-Back): the EPT paging structures themselves
   *   are cached in Write-Back mode.
   * page_walk_length = 3: we have 4 levels of tables, so 4 - 1 = 3.
   * dirty_access_enabled = 1: allow the hardware to set accessed and
   *   dirty bits in EPT entries automatically.
   * pml4_address: the host physical address (as a PFN) of our EPT PML4.
   */
  eptp->fields.dirty_access_enabled = 1;
  eptp->fields.memory_type = 6;      /* Write-Back */
  eptp->fields.page_walk_length = 3; /* 4 levels - 1 */
  eptp->fields.pml4_address = virtual_to_physical(ept_pml4) / PAGE_SIZE;
  eptp->fields.reserved1 = 0;
  eptp->fields.reserved2 = 0;

  pr_info("[*] Hyperion: EPT pointer allocated at 0x%llx\n", eptp->all);
  return eptp->all;
}
