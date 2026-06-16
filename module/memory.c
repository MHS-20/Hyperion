#include "hyperion.h"
#include <asm/io.h>
#include <asm/msr.h>
#include <linux/gfp.h>
#include <linux/mm.h>

#define VMXON_SIZE 4096
#define VMCS_SIZE 4096
#define ALIGNMENT_PAGE 4096

uint64_t virtual_to_physical(void *va) { return (uint64_t)virt_to_phys(va); }

void *physical_to_virtual(uint64_t pa) { return phys_to_virt((phys_addr_t)pa); }

/* IA32_VMX_BASIC MSR layout */
union ia32_vmx_basic_msr {
  uint64_t all;
  struct {
    uint32_t revision_identifier : 31; /* [0-30]  */
    uint32_t reserved1 : 1;            /* [31]    */
    uint32_t region_size : 12;         /* [32-43] */
    uint32_t region_clear : 1;         /* [44]    */
    uint32_t reserved2 : 3;            /* [45-47] */
    uint32_t supported_ia64 : 1;       /* [48]    */
    uint32_t supported_dual : 1;       /* [49]    */
    uint32_t memory_type : 4;          /* [50-53] */
    uint32_t vm_exit_report : 1;       /* [54]    */
    uint32_t vmx_capability_hint : 1;  /* [55]    */
    uint32_t reserved3 : 8;            /* [56-63] */
  } fields;
};

bool allocate_vmxon_region(struct virtual_machine_state *guest_state) {
  union ia32_vmx_basic_msr basic;
  void *buffer;
  uint64_t phys_buffer;
  uint64_t aligned_phys;
  void *aligned_virt;
  uint8_t status;

  /*
   * Allocate a contiguous physically-aligned 4KB region.
   * We allocate 2x the size to guarantee we can find a 4KB-aligned
   * address within the allocation even if the allocator doesn't
   * return a page-aligned pointer.
   */
  buffer = kmalloc(2 * VMXON_SIZE, GFP_KERNEL);
  if (!buffer) {
    printk(KERN_ERR "[*] Hyperion: failed to allocate VMXON region\n");
    return false;
  }

  /* Zero out the allocation */
  memset(buffer, 0, 2 * VMXON_SIZE);

  phys_buffer = virtual_to_physical(buffer);

  /* Align to 4KB boundary */
  aligned_phys =
      (phys_buffer + ALIGNMENT_PAGE - 1) & ~(uint64_t)(ALIGNMENT_PAGE - 1);
  aligned_virt = physical_to_virtual(aligned_phys);

  printk(KERN_INFO "[*] Hyperion: VMXON virtual buffer @ 0x%llx\n",
         (uint64_t)buffer);
  printk(KERN_INFO "[*] Hyperion: VMXON aligned virtual @ 0x%llx\n",
         (uint64_t)aligned_virt);
  printk(KERN_INFO "[*] Hyperion: VMXON aligned physical @ 0x%llx\n",
         aligned_phys);

  /*
   * From Intel SDM 24.11.5:
   * Before executing VMXON, write the VMCS revision identifier
   * to bits 30:0 of the first 4 bytes of the VMXON region.
   * Bit 31 must be cleared to 0.
   */
  rdmsrl(MSR_IA32_VMX_BASIC, basic.all);
  printk(KERN_INFO "[*] Hyperion: IA32_VMX_BASIC revision identifier: 0x%x\n",
         basic.fields.revision_identifier);

  *(uint32_t *)aligned_virt = basic.fields.revision_identifier;

  /* Execute VMXON with the physical address of our region */
  __asm__ volatile("vmxon %1\n\t"
                   "setna %0\n\t" /* status = CF | ZF; 0 = success */
                   : "=qm"(status)
                   : "m"(aligned_phys)
                   : "cc", "memory");

  if (status) {
    printk(KERN_ERR "[*] Hyperion: VMXON failed with status %d\n", status);
    kfree(buffer);
    return false;
  }

  guest_state->vmxon_region = aligned_phys;
  return true;
}

bool allocate_vmcs_region(struct virtual_machine_state *guest_state) {
  union ia32_vmx_basic_msr basic;
  void *buffer;
  uint64_t phys_buffer;
  uint64_t aligned_phys;
  void *aligned_virt;
  uint8_t status;

  buffer = kmalloc(2 * VMCS_SIZE, GFP_KERNEL);
  if (!buffer) {
    printk(KERN_ERR "[*] Hyperion: failed to allocate VMCS region\n");
    return false;
  }

  memset(buffer, 0, 2 * VMCS_SIZE);

  phys_buffer = virtual_to_physical(buffer);
  aligned_phys =
      (phys_buffer + ALIGNMENT_PAGE - 1) & ~(uint64_t)(ALIGNMENT_PAGE - 1);
  aligned_virt = physical_to_virtual(aligned_phys);

  printk(KERN_INFO "[*] Hyperion: VMCS virtual buffer @ 0x%llx\n",
         (uint64_t)buffer);
  printk(KERN_INFO "[*] Hyperion: VMCS aligned virtual @ 0x%llx\n",
         (uint64_t)aligned_virt);
  printk(KERN_INFO "[*] Hyperion: VMCS aligned physical @ 0x%llx\n",
         aligned_phys);

  /* Write the VMCS revision identifier before VMPTRLD */
  rdmsrl(MSR_IA32_VMX_BASIC, basic.all);
  *(uint32_t *)aligned_virt = basic.fields.revision_identifier;

  /* VMPTRLD: make this VMCS current and active */
  __asm__ volatile("vmptrld %1\n\t"
                   "setna %0\n\t"
                   : "=qm"(status)
                   : "m"(aligned_phys)
                   : "cc", "memory");

  if (status) {
    printk(KERN_ERR "[*] Hyperion: VMPTRLD failed with status %d\n", status);
    kfree(buffer);
    return false;
  }

  guest_state->vmcs_region = aligned_phys;
  return true;
}
