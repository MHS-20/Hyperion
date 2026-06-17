#include "hyperion.h"
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/smp.h>

int processor_count = 0;
struct virtual_machine_state *g_guest_state = NULL;

uint64_t g_stack_pointer_for_returning;
uint64_t g_base_pointer_for_returning;

/* IA32_FEATURE_CONTROL MSR layout */
union ia32_feature_control_msr {
  uint64_t all;
  struct {
    uint64_t lock : 1;          /* [0]    Lock bit */
    uint64_t enable_smx : 1;    /* [1]    Enable VMX in SMX */
    uint64_t enable_vmxon : 1;  /* [2]    Enable VMXON outside SMX */
    uint64_t reserved2 : 5;     /* [3-7]  */
    uint64_t local_senter : 7;  /* [8-14] */
    uint64_t global_senter : 1; /* [15]   */
    uint64_t reserved3 : 48;    /* [16-63]*/
  } fields;
};

static void asm_save_state_for_vmexit(void) {
  __asm__ volatile("mov %%rsp, %0\n\t"
                   "mov %%rbp, %1\n\t"
                   : "=m"(g_stack_pointer_for_returning),
                     "=m"(g_base_pointer_for_returning)
                   :
                   : "memory");
}

bool is_vmx_supported(void) {
  uint32_t ecx;
  union ia32_feature_control_msr ctrl;

  /* Check CPUID.1:ECX[5] — VMX support bit */
  __asm__ volatile("mov $1, %%eax\n\t"
                   "cpuid\n\t"
                   : "=c"(ecx)
                   :
                   : "eax", "ebx", "edx");

  if (!(ecx & (1 << 5))) {
    printk(KERN_ERR "[*] Hyperion: CPUID reports no VMX support\n");
    return false;
  }

  /* Check IA32_FEATURE_CONTROL MSR */
  rdmsrl(MSR_IA32_FEAT_CTL, ctrl.all);

  if (!ctrl.fields.lock) {
    /* Lock bit not set — we can write to this MSR to enable VMX */
    ctrl.fields.lock = 1;
    ctrl.fields.enable_vmxon = 1;
    wrmsrl(MSR_IA32_FEAT_CTL, ctrl.all);
  } else if (!ctrl.fields.enable_vmxon) {
    printk(KERN_ERR "[*] Hyperion: VMX locked off in BIOS\n");
    return false;
  }

  return true;
}

static void vmx_init_on_cpu(void *info) {
  int cpu = smp_processor_id();
  printk(KERN_INFO "[*] Hyperion: initializing VMX on CPU %d\n", cpu);

  enable_vmx_operation();

  /* Ensure VMX is off before trying to turn it on.
   * VMXOFF will #UD if VMX is not active; the exception table entry
   * makes the kernel skip faulting VMXOFF silently. */
  __asm__ volatile("1: vmxoff\n\t"
                   "2:\n\t" _ASM_EXTABLE(1b, 2b)::
                       : "cc");

  if (!allocate_vmxon_region(&g_guest_state[cpu])) {
    printk(KERN_ERR "[*] Hyperion: VMXON region allocation failed on CPU %d\n",
           cpu);
    return;
  }

  if (!allocate_vmcs_region(&g_guest_state[cpu])) {
    printk(KERN_ERR "[*] Hyperion: VMCS region allocation failed on CPU %d\n",
           cpu);
    return;
  }

  printk(KERN_INFO "[*] Hyperion: CPU %d — VMXON region @ 0x%llx\n", cpu,
         g_guest_state[cpu].vmxon_region);
  printk(KERN_INFO "[*] Hyperion: CPU %d — VMCS region  @ 0x%llx\n", cpu,
         g_guest_state[cpu].vmcs_region);

  /*
   * Allocate a dedicated stack for the VM-exit handler.
   * VMM_STACK_SIZE bytes, zeroed, from the kernel heap.
   */
  g_guest_state[cpu].vmm_stack_virt = kmalloc(VMM_STACK_SIZE, GFP_KERNEL);
  if (!g_guest_state[cpu].vmm_stack_virt) {
    printk(KERN_ERR "[*] Hyperion: failed to allocate VMM stack\n");
    return;
  }
  memset(g_guest_state[cpu].vmm_stack_virt, 0, VMM_STACK_SIZE);

  /*
   * HOST_RSP in the VMCS must point to the *top* of the stack since
   * the stack grows downward on x86_64.
   */
  g_guest_state[cpu].vmm_stack =
      (uint64_t)g_guest_state[cpu].vmm_stack_virt + VMM_STACK_SIZE - 1;

  /*
   * Allocate a 4KB page for the MSR Bitmap.
   * All zeros means all MSR accesses cause VM-exits.
   */
  g_guest_state[cpu].msr_bitmap_virt = kmalloc(PAGE_SIZE, GFP_KERNEL);
  if (!g_guest_state[cpu].msr_bitmap_virt) {
    printk(KERN_ERR "[*] Hyperion: failed to allocate MSR Bitmap\n");
    return;
  }
  memset(g_guest_state[cpu].msr_bitmap_virt, 0, PAGE_SIZE);

  g_guest_state[cpu].msr_bitmap = (uint64_t)g_guest_state[cpu].msr_bitmap_virt;
  g_guest_state[cpu].msr_bitmap_physical =
      virtual_to_physical(g_guest_state[cpu].msr_bitmap_virt);

  if (!clear_vmcs_state(&g_guest_state[cpu])) {
    printk(KERN_ERR "[*] Hyperion: VMCLEAR failed on CPU %d\n", cpu);
    return;
  }

  if (!allocate_vmcs_region(&g_guest_state[cpu])) {
    printk(KERN_ERR "[*] Hyperion: VMPTRLD failed on CPU %d\n", cpu);
    return;
  }

  g_guest_state[cpu].eptp = initialize_eptp();
  if (!g_guest_state[cpu].eptp) {
    printk(KERN_ERR "[*] Hyperion: EPTP init failed on CPU %d\n", cpu);
    return;
  }
}

bool initialize_vmx(void) {
  if (!is_vmx_supported()) {
    printk(KERN_ERR "[*] Hyperion: VMX is not supported on this machine\n");
    return false;
  }

  processor_count = num_online_cpus();
  g_guest_state = kzalloc(
      sizeof(struct virtual_machine_state) * processor_count, GFP_KERNEL);
  if (!g_guest_state) {
    printk(KERN_ERR "[*] Hyperion: failed to allocate guest state array\n");
    return false;
  }

  /* Run vmx_init_on_cpu() on every online logical processor */
  on_each_cpu(vmx_init_on_cpu, NULL, 1);

  return true;
}

static void vmx_exit_on_cpu(void *info) {
  int cpu = smp_processor_id();
  __asm__ volatile("vmxoff\n\t" ::: "cc");
  printk(KERN_INFO "[*] Hyperion: VMX turned off on CPU %d\n", cpu);
}

void terminate_vmx(void) {
  int i;

  printk(KERN_INFO "[*] Hyperion: terminating VMX on all CPUs\n");

  on_each_cpu(vmx_exit_on_cpu, NULL, 1);

  /* Free all per-CPU regions */
  for (i = 0; i < processor_count; i++) {
    if (g_guest_state[i].vmxon_alloc)
      kfree(g_guest_state[i].vmxon_alloc);

    if (g_guest_state[i].vmcs_alloc)
      kfree(g_guest_state[i].vmcs_alloc);
  }

  kfree(g_guest_state);
  g_guest_state = NULL;

  printk(KERN_INFO "[*] Hyperion: VMX terminated successfully\n");
}

uint64_t vmptrst_instruction(void) {
  uint64_t vmcs_pa = 0;

  __asm__ volatile("vmptrst %0\n\t" : "=m"(vmcs_pa) : : "cc", "memory");

  printk(KERN_INFO "[*] Hyperion: VMPTRST %llx\n", vmcs_pa);

  return vmcs_pa;
}

/*
 * VMWRITE wrapper.
 * field: VMCS field encoding (see the VMCS_FIELDS enum).
 * value: the value to write to that field.
 */
static void vmwrite(uint64_t field, uint64_t value) {
  __asm__ volatile("vmwrite %1, %0\n\t"
                   :
                   : "rm"(field), "r"(value)
                   : "cc", "memory");
}

/*
 * VMREAD wrapper.
 * field:  VMCS field encoding to read.
 * value:  pointer to where the read value is stored.
 */
static void vmread(uint64_t field, uint64_t *value) {
  __asm__ volatile("vmread %1, %0\n\t"
                   : "=rm"(*value)
                   : "r"(field)
                   : "cc", "memory");
}

/* Reading the GDT Base and Limit */

struct gdtr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

static uint64_t get_gdt_base(void) {
  struct gdtr gdtr;
  __asm__ volatile("sgdt %0" : "=m"(gdtr));
  return gdtr.base;
}

static uint16_t get_gdt_limit(void) {
  struct gdtr gdtr;
  __asm__ volatile("sgdt %0" : "=m"(gdtr));
  return gdtr.limit;
}

/* Reading the IDT Base and Limit */
struct idtr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

static uint64_t get_idt_base(void) {
  struct idtr idtr;
  __asm__ volatile("sidt %0" : "=m"(idtr));
  return idtr.base;
}

static uint16_t get_idt_limit(void) {
  struct idtr idtr;
  __asm__ volatile("sidt %0" : "=m"(idtr));
  return idtr.limit;
}

/* Reading Segment Registers and System Table Registers */
static uint16_t get_cs(void) {
  uint16_t v;
  __asm__("mov %%cs, %0" : "=r"(v));
  return v;
}
static uint16_t get_ds(void) {
  uint16_t v;
  __asm__("mov %%ds, %0" : "=r"(v));
  return v;
}
static uint16_t get_es(void) {
  uint16_t v;
  __asm__("mov %%es, %0" : "=r"(v));
  return v;
}
static uint16_t get_ss(void) {
  uint16_t v;
  __asm__("mov %%ss, %0" : "=r"(v));
  return v;
}
static uint16_t get_fs(void) {
  uint16_t v;
  __asm__("mov %%fs, %0" : "=r"(v));
  return v;
}
static uint16_t get_gs(void) {
  uint16_t v;
  __asm__("mov %%gs, %0" : "=r"(v));
  return v;
}
static uint16_t get_ldtr(void) {
  uint16_t v;
  __asm__("sldt %0" : "=r"(v));
  return v;
}
static uint16_t get_tr(void) {
  uint16_t v;
  __asm__("str %0" : "=r"(v));
  return v;
}

/* Reading Segment Registers and System Table Registers */
static uint64_t get_rflags(void) {
  uint64_t rflags;
  __asm__ volatile("pushfq\n\t"
                   "pop %0\n\t"
                   : "=r"(rflags)
                   :
                   : "cc");
  return rflags;
}

/* Intel segment descriptor (8 bytes) */
struct segment_descriptor {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_mid;
  uint8_t attrs_low;        /* accessed, type, DPL, present */
  uint8_t limit_high_attrs; /* limit[19:16] + AVL + L + D/B + G */
  uint8_t base_high;
} __attribute__((packed));

/* Parsed representation of a segment */
struct segment_selector {
  uint16_t sel;
  uint64_t base;
  uint32_t limit;
  uint32_t attributes;
};

/*
 * Read a single segment descriptor from the GDT and fill in a
 * segment_selector structure.  Returns true on success.
 */
static bool get_segment_descriptor(struct segment_selector *seg,
                                   uint16_t selector, unsigned char *gdt_base) {
  struct segment_descriptor *desc;

  if (!seg)
    return false;

  /* Bits 0-1 = RPL, bit 2 = table indicator (must be 0 for GDT) */
  if (selector & 0x4)
    return false;

  desc = (struct segment_descriptor *)(gdt_base + (selector & ~0x7));

  seg->sel = selector;
  seg->base = desc->base_low | ((uint32_t)desc->base_mid << 16) |
              ((uint32_t)desc->base_high << 24);
  seg->limit =
      desc->limit_low | ((uint32_t)(desc->limit_high_attrs & 0xf) << 16);
  seg->attributes =
      desc->attrs_low | ((uint32_t)(desc->limit_high_attrs & 0xf0) << 4);

  /*
   * If the system descriptor bit (bit 4 of attrs_low) is cleared,
   * this is a 16-byte descriptor (TSS, call gate, etc.) and the
   * upper 32 bits of the base are in the second 8 bytes.
   */
  if (!(desc->attrs_low & 0x10)) {
    uint64_t *extra = (uint64_t *)((unsigned char *)desc + 8);
    seg->base = (seg->base & 0xffffffff) | ((*extra & 0xffffffff) << 32);
  }

  /* If the Granularity bit is set, scale limit to 4KB pages */
  if (seg->attributes & 0x8000)
    seg->limit = (seg->limit << 12) + 0xfff;

  return true;
}

/*
 * Fill VMCS guest-state fields for a single segment register.
 * SegmentReg: one of ES=0, CS=1, SS=2, DS=3, FS=4, GS=5, LDTR=6, TR=7
 * (these indices match the VMCS field encoding layout).
 */
enum seg_reg {
  SEG_ES = 0,
  SEG_CS = 1,
  SEG_SS = 2,
  SEG_DS = 3,
  SEG_FS = 4,
  SEG_GS = 5,
  SEG_LDTR = 6,
  SEG_TR = 7,
};

static void fill_guest_selector_data(void *gdt_base, enum seg_reg seg_reg,
                                     uint16_t selector) {
  struct segment_selector seg = {0};
  uint32_t access_rights;

  get_segment_descriptor(&seg, selector, (unsigned char *)gdt_base);

  /*
   * The access rights field in the VMCS is laid out as:
   *   bits  3:0  = type
   *   bit   4    = S (system) — descriptor type
   *   bits  6:5  = DPL
   *   bit   7    = P (present)
   *   bits 11:8  = reserved (set to 0)
   *   bit  12    = AVL
   *   bit  13    = reserved (for code/data, this is 0; for system, L flag)
   *   bit  14    = D/B (default operation size)
   *   bit  15    = G (granularity)
   *   bit  16    = unusable (if selector is NULL, this bit must be set)
   *
   * The lower byte of seg.attributes = attrs_low  (type, S, DPL, P)
   * The upper bits (12..15) come from the second attribute byte.
   */
  access_rights = seg.attributes & 0xf0ff;

  if (!selector)
    access_rights |= 0x10000; /* mark as unusable */

  vmwrite(GUEST_ES_SELECTOR + seg_reg * 2, selector);
  vmwrite(GUEST_ES_LIMIT + seg_reg * 2, seg.limit);
  vmwrite(GUEST_ES_AR_BYTES + seg_reg * 2, access_rights);
  vmwrite(GUEST_ES_BASE + seg_reg * 2, seg.base);
}

// Sanitizing VMX Control Values
static uint32_t adjust_controls(uint32_t ctl, uint32_t msr) {
  uint64_t msr_value;

  rdmsrl(msr, msr_value);

  ctl &= (uint32_t)(msr_value >> 32);        /* high word: must be zero */
  ctl |= (uint32_t)(msr_value & 0xFFFFFFFF); /* low word: must be one */

  return ctl;
}

static bool setup_vmcs(struct virtual_machine_state *guest_state,
                       uint64_t eptp_val) {
  uint64_t gdt_base;
  struct segment_selector tr_selector = {0};

  /* ============= HOST SEGMENT SELECTORS ============= */
  vmwrite(HOST_ES_SELECTOR, get_es() & 0xF8);
  vmwrite(HOST_CS_SELECTOR, get_cs() & 0xF8);
  vmwrite(HOST_SS_SELECTOR, get_ss() & 0xF8);
  vmwrite(HOST_DS_SELECTOR, get_ds() & 0xF8);
  vmwrite(HOST_FS_SELECTOR, get_fs() & 0xF8);
  vmwrite(HOST_GS_SELECTOR, get_gs() & 0xF8);
  vmwrite(HOST_TR_SELECTOR, get_tr() & 0xF8);

  /* ============= VMCS LINK POINTER ============= */
  vmwrite(VMCS_LINK_POINTER, ~0ULL);

  /* ============= GUEST IA32_DEBUGCTL ============= */
  {
    uint64_t debug_ctl;
    rdmsrl(MSR_IA32_DEBUGCTL, debug_ctl);
    vmwrite(GUEST_IA32_DEBUGCTL, debug_ctl & 0xFFFFFFFF);
    vmwrite(GUEST_IA32_DEBUGCTL_HIGH, debug_ctl >> 32);
  }

  /* ============= ZERO-FILL FIELDS ============= */
  vmwrite(TSC_OFFSET, 0);
  vmwrite(TSC_OFFSET_HIGH, 0);
  vmwrite(PAGE_FAULT_ERROR_CODE_MASK, 0);
  vmwrite(PAGE_FAULT_ERROR_CODE_MATCH, 0);
  vmwrite(CR3_TARGET_COUNT, 0);
  vmwrite(VM_EXIT_MSR_STORE_COUNT, 0);
  vmwrite(VM_EXIT_MSR_LOAD_COUNT, 0);
  vmwrite(VM_ENTRY_MSR_LOAD_COUNT, 0);
  vmwrite(VM_ENTRY_INTR_INFO_FIELD, 0);

  /* ============= GUEST SEGMENT DATA ============= */
  gdt_base = get_gdt_base();

  fill_guest_selector_data((void *)gdt_base, SEG_ES, get_es());
  fill_guest_selector_data((void *)gdt_base, SEG_CS, get_cs());
  fill_guest_selector_data((void *)gdt_base, SEG_SS, get_ss());
  fill_guest_selector_data((void *)gdt_base, SEG_DS, get_ds());
  fill_guest_selector_data((void *)gdt_base, SEG_FS, get_fs());
  fill_guest_selector_data((void *)gdt_base, SEG_GS, get_gs());
  fill_guest_selector_data((void *)gdt_base, SEG_LDTR, get_ldtr());
  fill_guest_selector_data((void *)gdt_base, SEG_TR, get_tr());

  /* ============= GUEST FS/GS BASE ============= */
  {
    uint64_t fs_base, gs_base;
    rdmsrl(MSR_FS_BASE, fs_base);
    rdmsrl(MSR_GS_BASE, gs_base);
    vmwrite(GUEST_FS_BASE, fs_base);
    vmwrite(GUEST_GS_BASE, gs_base);
  }

  /* ============= GUEST INTERRUPTIBILITY AND ACTIVITY ============= */
  vmwrite(GUEST_INTERRUPTIBILITY_INFO, 0);
  vmwrite(GUEST_ACTIVITY_STATE, 0); /* VMCS activity = active */

  /* ============= VMX EXECUTION CONTROLS ============= */
  vmwrite(CPU_BASED_VM_EXEC_CONTROL,
          adjust_controls(CPU_BASED_HLT_EXITING |
                              CPU_BASED_ACTIVATE_SECONDARY_CONTROLS,
                          MSR_IA32_VMX_PROCBASED_CTLS));

  vmwrite(
      SECONDARY_VM_EXEC_CONTROL,
      adjust_controls(CPU_BASED_CTL2_RDTSCP /* | CPU_BASED_CTL2_ENABLE_EPT */,
                      MSR_IA32_VMX_PROCBASED_CTLS2));

  /* ============= CONTROL REGISTERS AND DR7 ============= */
  {
    unsigned long cr0, cr3, cr4, dr7;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %%dr7, %0" : "=r"(dr7));

    vmwrite(GUEST_CR0, cr0);
    vmwrite(GUEST_CR3, cr3);
    vmwrite(GUEST_CR4, cr4);
    vmwrite(GUEST_DR7, dr7);

    vmwrite(HOST_CR0, cr0);
    vmwrite(HOST_CR3, cr3);
    vmwrite(HOST_CR4, cr4);
  }

  /* ============= GDT & IDT ============= */
  vmwrite(GUEST_GDTR_BASE, get_gdt_base());
  vmwrite(GUEST_IDTR_BASE, get_idt_base());
  vmwrite(GUEST_GDTR_LIMIT, get_gdt_limit());
  vmwrite(GUEST_IDTR_LIMIT, get_idt_limit());

  /* ============= RFLAGS ============= */
  vmwrite(GUEST_RFLAGS, get_rflags());

  /* ============= SYSENTER MSRs ============= */
  {
    uint64_t sysenter_cs, sysenter_eip, sysenter_esp;
    rdmsrl(MSR_IA32_SYSENTER_CS, sysenter_cs);
    rdmsrl(MSR_IA32_SYSENTER_EIP, sysenter_eip);
    rdmsrl(MSR_IA32_SYSENTER_ESP, sysenter_esp);

    vmwrite(GUEST_SYSENTER_CS, sysenter_cs);
    vmwrite(GUEST_SYSENTER_EIP, sysenter_eip);
    vmwrite(GUEST_SYSENTER_ESP, sysenter_esp);
    vmwrite(HOST_IA32_SYSENTER_CS, sysenter_cs);
    vmwrite(HOST_IA32_SYSENTER_EIP, sysenter_eip);
    vmwrite(HOST_IA32_SYSENTER_ESP, sysenter_esp);
  }

  /* ============= HOST BASE ADDRESSES ============= */
  {
    uint64_t fs_base, gs_base;
    rdmsrl(MSR_FS_BASE, fs_base);
    rdmsrl(MSR_GS_BASE, gs_base);

    /* HOST_TR_BASE requires parsing the TSS descriptor from the GDT */
    get_segment_descriptor(&tr_selector, get_tr(),
                           (unsigned char *)get_gdt_base());
    vmwrite(HOST_TR_BASE, tr_selector.base);

    vmwrite(HOST_FS_BASE, fs_base);
    vmwrite(HOST_GS_BASE, gs_base);
    vmwrite(HOST_GDTR_BASE, get_gdt_base());
    vmwrite(HOST_IDTR_BASE, get_idt_base());
  }

  /* ============= GUEST/HOST RSP AND RIP ============= */
  vmwrite(GUEST_RSP, guest_state->vmcs_region); /* placeholder */
  vmwrite(GUEST_RIP, guest_state->vmcs_region); /* placeholder */

  vmwrite(HOST_RSP, guest_state->vmm_stack);
  vmwrite(HOST_RIP, (uint64_t)asm_vmexit_handler);

  /* ============= MSR BITMAP ============= */
  vmwrite(MSR_BITMAP, guest_state->msr_bitmap_physical);
  vmwrite(MSR_BITMAP_HIGH, guest_state->msr_bitmap_physical >> 32);

  return true;
}

/*
 * C-level VM-exit handler.
 * Called from the assembly trampoline with RDI pointing to the
 * saved register area on the stack.
 */
void main_vmexit_handler(uint64_t *guest_regs) {
  uint64_t exit_reason = 0;
  uint64_t exit_qualification = 0;

  vmread(VM_EXIT_REASON, &exit_reason);
  vmread(EXIT_QUALIFICATION, &exit_qualification);

  exit_reason = exit_reason & 0xffff; /* low 16 bits are the basic reason */

  printk(KERN_INFO "[*] Hyperion: VM_EXIT_REASON 0x%llx\n", exit_reason);
  printk(KERN_INFO "[*] Hyperion: EXIT_QUALIFICATION 0x%llx\n",
         exit_qualification);

  switch (exit_reason) {

  /*
   * These instructions cause VM-exits unconditionally (Intel SDM 25.1.2).
   * If we reach here without one of these instructions explicitly executed
   * by the guest on purpose, something went wrong.  For now we ignore them.
   */
  case EXIT_REASON_VMCLEAR:
  case EXIT_REASON_VMPTRLD:
  case EXIT_REASON_VMPTRST:
  case EXIT_REASON_VMREAD:
  case EXIT_REASON_VMRESUME:
  case EXIT_REASON_VMWRITE:
  case EXIT_REASON_VMXOFF:
  case EXIT_REASON_VMXON:
  case EXIT_REASON_VMLAUNCH:
    break;

  case EXIT_REASON_HLT:
    printk(KERN_INFO "[*] Hyperion: HLT detected in guest — "
                     "stopping hypervisor\n");

    /* Turn off VMX and return to the pre-VMLAUNCH context */
    asm_vmxoff_and_restore_state();
    break;

  case EXIT_REASON_EXCEPTION_NMI:
  case EXIT_REASON_CPUID:
  case EXIT_REASON_INVD:
  case EXIT_REASON_VMCALL:
  case EXIT_REASON_CR_ACCESS:
  case EXIT_REASON_MSR_READ:
  case EXIT_REASON_MSR_WRITE:
  case EXIT_REASON_EPT_VIOLATION:
    break;

  default:
    break;
  }
}
