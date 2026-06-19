#include "vmx.h"
#include "hyperion.h"
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/list.h>

#define VMM_STACK_SIZE (PAGE_SIZE * 2)

int processor_count = 0;
struct virtual_machine_state *g_guest_state = NULL;

uint64_t g_stack_pointer_for_returning;
uint64_t g_base_pointer_for_returning;

/* Assembly symbols from exit_handler.s */
extern void asm_vmexit_handler(void);
extern void asm_vmxoff_and_restore_state(void);

uint64_t g_GuestRIP = 0;
uint64_t g_GuestRSP = 0;

static uint64_t g_Cr3TargetCount = 0;

/* Forward declarations for static functions used before their definitions */
static void vmwrite(uint64_t field, uint64_t value);
static void vmread(uint64_t field, uint64_t *value);
static void resume_to_next_instruction(void);
static void vmx_launch_on_cpu(void *info);
static bool setup_vmcs(struct virtual_machine_state *guest_state,
                       uint64_t eptp_val);
static void
SetupVmcsAndVirtualizeMachine(struct virtual_machine_state *guest_state,
                              uint64_t EPTP, void *GuestStack);

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

  g_guest_state[cpu].vmm_stack_virt = kmalloc(VMM_STACK_SIZE, GFP_KERNEL);
  if (!g_guest_state[cpu].vmm_stack_virt) {
    printk(KERN_ERR "[*] Hyperion: failed to allocate VMM stack\n");
    return;
  }
  memset(g_guest_state[cpu].vmm_stack_virt, 0, VMM_STACK_SIZE);

  g_guest_state[cpu].vmm_stack =
      (uint64_t)g_guest_state[cpu].vmm_stack_virt + VMM_STACK_SIZE - 1;

  g_guest_state[cpu].msr_bitmap_virt = kmalloc(PAGE_SIZE, GFP_KERNEL);
  if (!g_guest_state[cpu].msr_bitmap_virt) {
    printk(KERN_ERR "[*] Hyperion: failed to allocate MSR Bitmap\n");
    return;
  }
  memset(g_guest_state[cpu].msr_bitmap_virt, 0, PAGE_SIZE);

  g_guest_state[cpu].msr_bitmap = (uint64_t)g_guest_state[cpu].msr_bitmap_virt;
  g_guest_state[cpu].msr_bitmap_physical =
      virtual_to_physical(g_guest_state[cpu].msr_bitmap_virt);

  g_guest_state[cpu].eptp = initialize_eptp();
}

static void vmx_launch_on_cpu(void *info) {
  int cpu = smp_processor_id();

  __asm__ volatile("call VmxSaveState\n\t"
                   :
                   : "D"(cpu), "S"(g_guest_state[cpu].eptp)
                   : "rax", "rcx", "rdx", "rbx",
                     "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
                     "cc", "memory");
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

  /* Phase 1: setup VMXON/VMCS/VMM stack/MSR bitmap/EPTP on all CPUs */
  on_each_cpu(vmx_init_on_cpu, NULL, 1);

  /* Phase 2: launch VMX non-root mode on all CPUs simultaneously */
  on_each_cpu(vmx_launch_on_cpu, NULL, 1);

  return true;
}

static void vmx_off_on_cpu(void *info) {
  int cpu = smp_processor_id();
  int CpuInfo[4] = {0};

  __asm__ volatile("mov %[eax_val], %%eax\n\t"
                   "mov %[ecx_val], %%ecx\n\t"
                   "cpuid\n\t"
                   : "=a"(CpuInfo[0]), "=b"(CpuInfo[1]),
                     "=c"(CpuInfo[2]), "=d"(CpuInfo[3])
                   : [eax_val] "r"(0x41414141),
                     [ecx_val] "r"(0x42424242)
                   : "memory");

  printk(KERN_INFO "[*] Hyperion: VMX turned off on CPU %d\n", cpu);
}

void terminate_vmx(void) {
  int i;

  printk(KERN_INFO "[*] Hyperion: terminating VMX on all CPUs\n");

  on_each_cpu(vmx_off_on_cpu, NULL, 1);

  for (i = 0; i < processor_count; i++) {
    if (g_guest_state[i].vmxon_alloc)
      kfree(g_guest_state[i].vmxon_alloc);

    if (g_guest_state[i].vmcs_alloc)
      kfree(g_guest_state[i].vmcs_alloc);

    if (g_guest_state[i].vmm_stack_virt)
      kfree(g_guest_state[i].vmm_stack_virt);

    if (g_guest_state[i].msr_bitmap_virt)
      kfree(g_guest_state[i].msr_bitmap_virt);
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
   * seg.attributes encodes G/D/B/L/AVL at bits 11:8.
   * Shift them up to bits 15:12 where the VMCS expects them.
   * For CS: preserve the L bit (bit 13) from the descriptor — it must be 1
   *   for a 64-bit code segment in IA-32e mode.
   * For SS, DS, ES, FS, GS, LDTR: clear L (bit 13 must be 0).
   * For TR: set L (bit 13) for 64-bit TSS (types 9/B).
   */
  access_rights = (seg.attributes & 0xff) |
                  ((seg.attributes & 0x0f00) << 4);

  if (seg_reg != SEG_CS && seg_reg != SEG_TR)
    access_rights &= ~(1 << 13);

  if (!(access_rights & (1 << 4))) {
    uint8_t type = access_rights & 0xF;
    if (type == 0x9 || type == 0xB)
      access_rights |= (1 << 13);
  }

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
  vmwrite(GUEST_IA32_DEBUGCTL, 0);
  vmwrite(GUEST_IA32_DEBUGCTL_HIGH, 0);

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
  /*
   * Primary Processor-Based VM-Execution Controls:
   *   CPU_BASED_ACTIVATE_MSR_BITMAP   — enable MSR-bitmap filtering
   *   CPU_BASED_ACTIVATE_SECONDARY_CONTROLS — enable secondary controls
   */
  vmwrite(CPU_BASED_VM_EXEC_CONTROL,
          adjust_controls(CPU_BASED_ACTIVATE_MSR_BITMAP |
                              CPU_BASED_ACTIVATE_SECONDARY_CONTROLS,
                          MSR_IA32_VMX_PROCBASED_CTLS));

  /*
   * Secondary Processor-Based VM-Execution Controls:
   *   CPU_BASED_CTL2_RDTSCP              — enable RDTSCP instruction
   *   CPU_BASED_CTL2_ENABLE_EPT          — enable Extended Page Tables (if available)
   *   CPU_BASED_CTL2_ENABLE_INVPCID      — enable INVPCID instruction
   *   CPU_BASED_CTL2_ENABLE_XSAVE_XRSTORS — enable XSAVE/XRSTORS
   */
  if (g_ept_state && g_ept_state->EptPointer.all) {
    vmwrite(SECONDARY_VM_EXEC_CONTROL,
            adjust_controls(CPU_BASED_CTL2_RDTSCP |
                                CPU_BASED_CTL2_ENABLE_EPT |
                                CPU_BASED_CTL2_ENABLE_INVPCID |
                                CPU_BASED_CTL2_ENABLE_XSAVE_XRSTORS,
                            MSR_IA32_VMX_PROCBASED_CTLS2));
  } else {
    vmwrite(SECONDARY_VM_EXEC_CONTROL,
            adjust_controls(CPU_BASED_CTL2_RDTSCP |
                                CPU_BASED_CTL2_ENABLE_INVPCID |
                                CPU_BASED_CTL2_ENABLE_XSAVE_XRSTORS,
                            MSR_IA32_VMX_PROCBASED_CTLS2));
  }

  // Pin-based controls
  vmwrite(PIN_BASED_VM_EXEC_CONTROL,
          adjust_controls(0, MSR_IA32_VMX_PINBASED_CTLS));

  // VM-exit controls: 64-bit (IA-32e) mode
  vmwrite(VM_EXIT_CONTROLS,
          adjust_controls(VM_EXIT_IA32E_MODE, MSR_IA32_VMX_EXIT_CTLS));

  // VM-entry controls: 64-bit (IA-32e) mode
  vmwrite(VM_ENTRY_CONTROLS,
          adjust_controls(VM_ENTRY_IA32E_MODE, MSR_IA32_VMX_ENTRY_CTLS));

  /* ============= CONTROL REGISTERS AND DR7 ============= */
  {
    unsigned long cr0, cr3, cr4, dr7;
    uint64_t cr0_fixed0, cr0_fixed1, cr4_fixed0, cr4_fixed1;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %%dr7, %0" : "=r"(dr7));

    /* Mask CR0/CR4 to the VMX-allowed range so the guest state
     * passes the VM-entry consistency checks even in nested VMX. */
    rdmsrl(MSR_IA32_VMX_CR0_FIXED0, cr0_fixed0);
    rdmsrl(MSR_IA32_VMX_CR0_FIXED1, cr0_fixed1);
    rdmsrl(MSR_IA32_VMX_CR4_FIXED0, cr4_fixed0);
    rdmsrl(MSR_IA32_VMX_CR4_FIXED1, cr4_fixed1);

    cr0 = (cr0 & cr0_fixed1) | cr0_fixed0;
    cr4 = (cr4 & cr4_fixed1) | cr4_fixed0;

    vmwrite(GUEST_CR0, cr0);
    vmwrite(GUEST_CR3, cr3);
    vmwrite(GUEST_CR4, cr4);
    vmwrite(GUEST_DR7, 0x400);

    vmwrite(HOST_CR0, cr0);
    vmwrite(HOST_CR3, cr3);
    vmwrite(HOST_CR4, cr4);

    vmwrite(CR0_GUEST_HOST_MASK, 0);
    vmwrite(CR4_GUEST_HOST_MASK, 0);
    vmwrite(CR0_READ_SHADOW, cr0);
    vmwrite(CR4_READ_SHADOW, cr4);
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

  /* ============= IA32_PAT ============= */
  {
    uint64_t pat;
    rdmsrl(MSR_IA32_CR_PAT, pat);
    vmwrite(GUEST_IA32_PAT, pat);
    vmwrite(HOST_IA32_PAT, pat);
  }

  /* ============= IA32_EFER ============= */
  {
    uint64_t efer;
    rdmsrl(MSR_EFER, efer);
    vmwrite(GUEST_IA32_EFER, efer);
    vmwrite(HOST_IA32_EFER, efer);
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

  /* ============= EPT POINTER ============= */
  if (g_ept_state && g_ept_state->EptPointer.all) {
    vmwrite(EPT_POINTER, g_ept_state->EptPointer.all);
  }

  return true;
}

static bool SetTargetControls(uint64_t CR3, uint64_t Index) {
  if (Index >= 4)
    return false;

  if (CR3 == 0) {
    if (g_Cr3TargetCount <= 0)
      return false;
    g_Cr3TargetCount -= 1;
    if (Index == 0) vmwrite(CR3_TARGET_VALUE0, 0);
    if (Index == 1) vmwrite(CR3_TARGET_VALUE1, 0);
    if (Index == 2) vmwrite(CR3_TARGET_VALUE2, 0);
    if (Index == 3) vmwrite(CR3_TARGET_VALUE3, 0);
  } else {
    if (Index == 0) vmwrite(CR3_TARGET_VALUE0, CR3);
    if (Index == 1) vmwrite(CR3_TARGET_VALUE1, CR3);
    if (Index == 2) vmwrite(CR3_TARGET_VALUE2, CR3);
    if (Index == 3) vmwrite(CR3_TARGET_VALUE3, CR3);
    g_Cr3TargetCount += 1;
  }
  vmwrite(CR3_TARGET_COUNT, g_Cr3TargetCount);
  return true;
}

static void SetBit(void *Addr, uint64_t Bit, bool Set) {
  uint64_t Byte = Bit / 8;
  uint64_t N = 7 - (Bit % 8);
  uint8_t *Addr2 = (uint8_t *)Addr;
  if (Set)
    Addr2[Byte] |= (1 << N);
  else
    Addr2[Byte] &= ~(1 << N);
}

static uint8_t GetBit(void *Addr, uint64_t Bit) {
  uint64_t Byte = Bit / 8;
  uint64_t K = 7 - (Bit % 8);
  uint8_t *Addr2 = (uint8_t *)Addr;
  return Addr2[Byte] & (1 << K);
}

static bool SetMsrBitmap(uint64_t Msr, int ProcessID,
                         bool ReadDetection, bool WriteDetection) {
  if (!ReadDetection && !WriteDetection)
    return false;

  if (Msr <= 0x00001FFF) {
    if (ReadDetection)
      SetBit(g_guest_state[ProcessID].msr_bitmap_virt, Msr, true);
    if (WriteDetection)
      SetBit((uint8_t *)g_guest_state[ProcessID].msr_bitmap_virt + 2048,
             Msr, true);
  } else if (Msr >= 0xC0000000 && Msr <= 0xC0001FFF) {
    uint64_t HighOffset = Msr - 0xC0000000;
    if (ReadDetection)
      SetBit((uint8_t *)g_guest_state[ProcessID].msr_bitmap_virt + 1024,
             HighOffset, true);
    if (WriteDetection)
      SetBit((uint8_t *)g_guest_state[ProcessID].msr_bitmap_virt + 3072,
             HighOffset, true);
  } else {
    return false;
  }
  return true;
}

static void HandleMSRRead(PGUEST_REGS GuestRegs) {
  uint64_t MsrValue = 0;
  if ((GuestRegs->rcx <= 0x00001FFF) ||
      (GuestRegs->rcx >= 0xC0000000 && GuestRegs->rcx <= 0xC0001FFF))
    rdmsrl((uint32_t)(GuestRegs->rcx & 0xFFFFFFFF), MsrValue);
  else
    MsrValue = 0;
  GuestRegs->rax = MsrValue & 0xFFFFFFFF;
  GuestRegs->rdx = (MsrValue >> 32) & 0xFFFFFFFF;
}

static void HandleMSRWrite(PGUEST_REGS GuestRegs) {
  if ((GuestRegs->rcx <= 0x00001FFF) ||
      (GuestRegs->rcx >= 0xC0000000 && GuestRegs->rcx <= 0xC0001FFF)) {
    uint64_t MsrValue = (GuestRegs->rax & 0xFFFFFFFF) |
                        ((GuestRegs->rdx & 0xFFFFFFFF) << 32);
    wrmsrl((uint32_t)(GuestRegs->rcx & 0xFFFFFFFF), MsrValue);
  }
}

static void HandleControlRegisterAccess(PGUEST_REGS GuestState) {
  uint64_t ExitQualification = 0;
  uint64_t ControlRegister, AccessType, RegisterIndex;
  uint64_t *RegPtr = NULL;

  vmread(EXIT_QUALIFICATION, &ExitQualification);
  ControlRegister = ExitQualification & 0xF;
  AccessType = (ExitQualification >> 4) & 0x3;
  RegisterIndex = (ExitQualification >> 8) & 0xF;

  {
    static const uint8_t reg_to_offset[16] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 10, 11, 12, 13, 14, 15
    };
    uint64_t *regs = (uint64_t *)GuestState;
    RegPtr = &regs[reg_to_offset[RegisterIndex & 0xF]];
  }

  if (RegisterIndex == 4) {
    uint64_t RSP = 0;
    vmread(GUEST_RSP, &RSP);
    *RegPtr = RSP;
  }

  switch (AccessType) {
  case TYPE_MOV_TO_CR:
    switch (ControlRegister) {
    case 0:
      vmwrite(GUEST_CR0, *RegPtr);
      vmwrite(CR0_READ_SHADOW, *RegPtr);
      break;
    case 3:
      vmwrite(GUEST_CR3, (*RegPtr & ~(1ULL << 63)));
      break;
    case 4:
      vmwrite(GUEST_CR4, *RegPtr);
      vmwrite(CR4_READ_SHADOW, *RegPtr);
      break;
    }
    break;
  case TYPE_MOV_FROM_CR:
    switch (ControlRegister) {
    case 0: vmread(GUEST_CR0, RegPtr); break;
    case 3: vmread(GUEST_CR3, RegPtr); break;
    case 4: vmread(GUEST_CR4, RegPtr); break;
    }
    break;
  }
}

static int VmcallTest(uint64_t Param1, uint64_t Param2, uint64_t Param3) {
  printk(KERN_INFO "[*] Hyperion: VmcallTest called with "
                   "Param1=0x%llx Param2=0x%llx Param3=0x%llx\n",
         Param1, Param2, Param3);
  return 0;
}

int VmxVmcallHandler(uint64_t VmcallNumber, uint64_t OptionalParam1,
                     uint64_t OptionalParam2, uint64_t OptionalParam3) {
  int VmcallStatus = -1;

  switch (VmcallNumber) {
  case VMCALL_TEST:
    VmcallStatus = VmcallTest(OptionalParam1, OptionalParam2, OptionalParam3);
    break;
  case VMCALL_VMXOFF:
    VmxVmxoff();
    VmcallStatus = 0;
    break;
  case VMCALL_EXEC_HOOK_PAGE:
    if (EptVmxRootModePageHook((void *)OptionalParam1, true))
      VmcallStatus = 0;
    break;
  case VMCALL_INVEPT_SINGLE_CONTEXT:
    InveptSingleContext(OptionalParam1);
    VmcallStatus = 0;
    break;
  case VMCALL_INVEPT_ALL_CONTEXT:
    InveptAllContexts();
    VmcallStatus = 0;
    break;
  default:
    VmcallStatus = 0;
    break;
  }

  return VmcallStatus;
}

void VmxVmxoff(void) {
  int cpu = smp_processor_id();
  uint64_t GuestRSP = 0;
  uint64_t GuestRIP = 0;
  uint64_t GuestCr3 = 0;
  uint64_t ExitInstructionLength = 0;

  vmread(GUEST_CR3, &GuestCr3);
  asm volatile("mov %0, %%cr3" :: "r"(GuestCr3) : "memory");

  vmread(GUEST_RIP, &GuestRIP);
  vmread(GUEST_RSP, &GuestRSP);
  vmread(VM_EXIT_INSTRUCTION_LEN, &ExitInstructionLength);
  GuestRIP += ExitInstructionLength;

  g_guest_state[cpu].vmxoff_state.guest_rip = GuestRIP;
  g_guest_state[cpu].vmxoff_state.guest_rsp = GuestRSP;
  g_guest_state[cpu].vmxoff_state.is_vmxoff_executed = true;

  asm volatile("vmxoff" ::: "cc");
}

uint64_t HvReturnStackPointerForVmxoff(void) {
  return g_guest_state[smp_processor_id()].vmxoff_state.guest_rsp;
}

uint64_t HvReturnInstructionPointerForVmxoff(void) {
  return g_guest_state[smp_processor_id()].vmxoff_state.guest_rip;
}

static void VmxDpcBroadcastTerminate(void *info) {
  AsmVmxVmcall(VMCALL_VMXOFF, 0, 0, 0);
  kfree(g_guest_state[smp_processor_id()].vmxon_alloc);
  kfree(g_guest_state[smp_processor_id()].vmcs_alloc);
  kfree(g_guest_state[smp_processor_id()].vmm_stack_virt);
  kfree(g_guest_state[smp_processor_id()].msr_bitmap_virt);
}

void HvTerminateVmx(void) {
  on_each_cpu(VmxDpcBroadcastTerminate, NULL, 1);

  if (g_ept_state) {
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &g_ept_state->EptPageTable->DynamicSplitList) {
      VMM_EPT_DYNAMIC_SPLIT *split;
      split = list_entry(pos, VMM_EPT_DYNAMIC_SPLIT, DynamicSplitList);
      list_del(pos);
      kfree(split);
    }

    int order = get_order(sizeof(VMM_EPT_PAGE_TABLE));
    free_pages((unsigned long)g_ept_state->EptPageTable, order);
    kfree(g_ept_state);
    g_ept_state = NULL;
  }

  kfree(g_guest_state);
  g_guest_state = NULL;
}

static bool HandleCPUID(PGUEST_REGS state) {
  int CpuInfo[4] = {0};
  uint64_t Mode = 0;

  vmread(GUEST_CS_SELECTOR, &Mode);
  Mode = Mode & 0x3;

  if ((state->rax == 0x41414141) && (state->rcx == 0x42424242) && Mode == 0)
    return true;

  __asm__ volatile("mov %[leaf], %%eax\n\t"
                   "mov %[subleaf], %%ecx\n\t"
                   "cpuid\n\t"
                   : "=a"(CpuInfo[0]), "=b"(CpuInfo[1]),
                     "=c"(CpuInfo[2]), "=d"(CpuInfo[3])
                   : [leaf] "r"((int)state->rax),
                     [subleaf] "r"((int)state->rcx)
                   : "memory");

  if (state->rax == 1)
    CpuInfo[2] |= (1 << 31);
  else if (state->rax == 0x40000000) {
    CpuInfo[1] = 0x6e6f6972;
    CpuInfo[2] = 0x76486e6f;
    CpuInfo[3] = 0x00000000;
    CpuInfo[0] = 0x72657079;
  }

  state->rax = CpuInfo[0];
  state->rbx = CpuInfo[1];
  state->rcx = CpuInfo[2];
  state->rdx = CpuInfo[3];

  return false;
}

/*
 * C-level VM-exit handler.
 * Called from the assembly trampoline with RDI pointing to the
 * saved register area on the stack.
 */
uint8_t main_vmexit_handler(uint64_t *guest_regs) {
  uint64_t exit_reason = 0;
  PGUEST_REGS GuestRegs = (PGUEST_REGS)guest_regs;
  uint8_t should_terminate = 0;

  vmread(VM_EXIT_REASON, &exit_reason);

  exit_reason = exit_reason & 0xffff;

  printk_ratelimited(KERN_INFO "[*] Hyperion: VM_EXIT_REASON 0x%llx on CPU %d\n",
         exit_reason, smp_processor_id());

  switch (exit_reason) {

  case EXIT_REASON_VMCLEAR:
  case EXIT_REASON_VMPTRLD:
  case EXIT_REASON_VMPTRST:
  case EXIT_REASON_VMREAD:
  case EXIT_REASON_VMRESUME:
  case EXIT_REASON_VMWRITE:
  case EXIT_REASON_VMXOFF:
  case EXIT_REASON_VMXON:
  case EXIT_REASON_VMLAUNCH: {
    uint64_t RFLAGS = 0;
    vmread(GUEST_RFLAGS, &RFLAGS);
    vmwrite(GUEST_RFLAGS, RFLAGS | 0x1);
    break;
  }

  case EXIT_REASON_INVALID_GUEST_STATE: {
    uint64_t exit_qual = 0;
    uint64_t instr_error = 0;
    vmread(VM_INSTRUCTION_ERROR, &instr_error);
    vmread(EXIT_QUALIFICATION, &exit_qual);
    printk(KERN_ERR "[*] Hyperion: VM-entry failed (invalid guest state): "
                     "qual=0x%llx err=0x%llx\n",
           exit_qual, instr_error);
    break;
  }

  case EXIT_REASON_EXTERNAL_INTERRUPT: {
    uint64_t intr_info = 0;
    vmread(VM_EXIT_INTR_INFO, &intr_info);
    if (intr_info & (1ULL << 31))
      vmwrite(VM_ENTRY_INTR_INFO_FIELD, intr_info);
    break;
  }

  case EXIT_REASON_EXCEPTION_NMI: {
    uint64_t intr_info = 0;
    uint64_t error_code = 0;
    vmread(VM_EXIT_INTR_INFO, &intr_info);
    vmread(VM_EXIT_INTR_ERROR_CODE, &error_code);
    vmwrite(VM_ENTRY_INTR_INFO_FIELD, intr_info);
    vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, error_code);
    break;
  }

  case EXIT_REASON_RDTSC:
  case EXIT_REASON_RDTSCP:
    resume_to_next_instruction();
    break;

  case EXIT_REASON_VMCALL: {
    GuestRegs->rax = VmxVmcallHandler(GuestRegs->rdi, GuestRegs->rsi,
                                      GuestRegs->rdx, GuestRegs->rcx);
    resume_to_next_instruction();
    break;
  }

  case EXIT_REASON_HLT:
    printk(KERN_INFO "[*] Hyperion: HLT detected in guest — "
                     "stopping hypervisor\n");
    asm_vmxoff_and_restore_state();
    break;

  case EXIT_REASON_CPUID: {
    bool term = HandleCPUID(GuestRegs);
    if (term) {
      uint64_t ExitInstructionLength = 0;
      vmread(GUEST_RIP, &g_GuestRIP);
      vmread(GUEST_RSP, &g_GuestRSP);
      vmread(VM_EXIT_INSTRUCTION_LEN, &ExitInstructionLength);
      g_GuestRIP += ExitInstructionLength;
      should_terminate = 1;
    } else {
      resume_to_next_instruction();
    }
    break;
  }

  case EXIT_REASON_CR_ACCESS:
    HandleControlRegisterAccess(GuestRegs);
    resume_to_next_instruction();
    break;

  case EXIT_REASON_MSR_READ: {
    uint32_t ECX = GuestRegs->rcx & 0xFFFFFFFF;
    printk(KERN_INFO "[*] Hyperion: RDMSR (bitmap) : 0x%x\n", ECX);
    HandleMSRRead(GuestRegs);
    resume_to_next_instruction();
    break;
  }

  case EXIT_REASON_MSR_WRITE: {
    uint32_t ECX = GuestRegs->rcx & 0xFFFFFFFF;
    printk(KERN_INFO "[*] Hyperion: WRMSR (bitmap) : 0x%x\n", ECX);
    HandleMSRWrite(GuestRegs);
    resume_to_next_instruction();
    break;
  }

  case EXIT_REASON_EPT_VIOLATION: {
    uint64_t exit_qual = 0;
    uint64_t guest_phys = 0;
    vmread(EXIT_QUALIFICATION, &exit_qual);
    vmread(GUEST_PHYSICAL_ADDRESS, &guest_phys);

    if (EptHandleEptViolation(exit_qual, guest_phys))
      g_guest_state[smp_processor_id()].increment_rip = false;
    else
      resume_to_next_instruction();

    break;
  }

  case EXIT_REASON_EPT_MISCONFIG: {
    uint64_t guest_phys = 0;
    vmread(GUEST_PHYSICAL_ADDRESS, &guest_phys);
    printk(KERN_ERR "[*] Hyperion: EPT misconfiguration at GPA=0x%llx\n",
           guest_phys);
    break;
  }

  default:
    printk_ratelimited(KERN_WARNING "[*] Hyperion: unhandled VM_EXIT_REASON 0x%llx on CPU %d\n",
           exit_reason, smp_processor_id());
    break;
  }

  if (g_guest_state[smp_processor_id()].vmxoff_state.is_vmxoff_executed)
    return 1;

  return should_terminate;
}

static void resume_to_next_instruction(void) {
  uint64_t current_rip = 0;
  uint64_t exit_instruction_length = 0;

  vmread(GUEST_RIP, &current_rip);
  vmread(VM_EXIT_INSTRUCTION_LEN, &exit_instruction_length);

  vmwrite(GUEST_RIP, current_rip + exit_instruction_length);
}

void vm_resume_instruction(void) {
  uint8_t status;
  uint64_t error_code = 0;

  __asm__ volatile("vmresume\n\t"
                   "setna %0\n\t"
                   : "=qm"(status)
                   :
                   : "cc", "memory");

  if (status) {
    vmread(VM_INSTRUCTION_ERROR, &error_code);
    __asm__ volatile("vmxoff\n\t" ::: "cc");
    printk(KERN_ERR "[*] Hyperion: VMRESUME error: 0x%llx\n", error_code);
    __asm__ volatile("1: hlt; jmp 1b" ::: "memory");
  }
}

/*
 * VirtualizeCurrentSystem
 *
 * Called per-CPU from VmxSaveState.  Clears the VMCS state, loads the
 * current VMCS, configures all VMCS fields (including setting GUEST_RIP
 * = VmxRestoreState and GUEST_RSP = the saved RSP), and executes VMLAUNCH.
 *
 * Parameters (System V AMD64 ABI):
 *   RDI = ProcessorID   — which logical CPU this is
 *   RSI = EPTP          — Extended Page Table Pointer
 *   RDX = GuestStack    — the saved RSP to use as GUEST_RSP
 */
void VirtualizeCurrentSystem(int ProcessorID, uint64_t EPTP, void *GuestStack) {
  printk(KERN_INFO
         "\n========== Virtualizing Current System (CPU%d) ==========\n",
         ProcessorID);

  if (!clear_vmcs_state(&g_guest_state[ProcessorID])) {
    printk(KERN_ERR "[*] Hyperion: VMCLEAR failed on CPU%d\n", ProcessorID);
    return;
  }

  if (!allocate_vmcs_region(&g_guest_state[ProcessorID])) {
    printk(KERN_ERR "[*] Hyperion: VMPTRLD failed on CPU%d\n", ProcessorID);
    return;
  }

  {
    uint64_t msr_pin, msr_proc, msr_proc2, msr_entry, msr_exit;
    rdmsrl(MSR_IA32_VMX_PINBASED_CTLS, msr_pin);
    rdmsrl(MSR_IA32_VMX_PROCBASED_CTLS, msr_proc);
    rdmsrl(MSR_IA32_VMX_PROCBASED_CTLS2, msr_proc2);
    rdmsrl(MSR_IA32_VMX_ENTRY_CTLS, msr_entry);
    rdmsrl(MSR_IA32_VMX_EXIT_CTLS, msr_exit);
    printk(KERN_INFO "[*] Hyperion: MSR_CTLS CPU%d — "
                     "PIN=0x%llx PROC=0x%llx PROC2=0x%llx "
                     "ENTRY=0x%llx EXIT=0x%llx\n",
           ProcessorID, msr_pin, msr_proc, msr_proc2,
           msr_entry, msr_exit);
  }

  printk(KERN_INFO
         "[*] Hyperion: Setting up VMCS for current system (CPU%d).\n",
         ProcessorID);
  SetupVmcsAndVirtualizeMachine(&g_guest_state[ProcessorID], EPTP, GuestStack);

  /*
   * Optionally configure MSR Bitmap here.
   * Example: detect reads and writes to MSR 0xC0000082 (LSTAR).
   * SetMSRBitmap(0xc0000082, ProcessorID, TRUE, TRUE);
   */

  {
    uint64_t efer, cr0, cr4, entry_ctl, exit_ctl, pin_ctl, cpu_ctl, cpu2_ctl;
    uint64_t cs_sel, cs_base, cs_limit, cs_ar;
    uint64_t ss_sel, ss_base, ss_limit, ss_ar;
    uint64_t tr_sel, tr_base, tr_limit, tr_ar;
    uint64_t ldtr_sel, ldtr_base, ldtr_limit, ldtr_ar;
    uint64_t ds_sel, ds_ar, es_sel, es_ar, fs_sel, fs_ar, gs_sel, gs_ar;
    uint64_t host_cr0, host_cr4, host_rip, host_rsp, host_efer, host_cs;
    uint64_t g_pat, g_rflags, g_dr7, vmcs_link;
    uint64_t cr4_fixed0, cr4_fixed1, cr0_fixed0, cr0_fixed1;
    vmread(GUEST_IA32_EFER, &efer);
    vmread(GUEST_CR0, &cr0);
    vmread(GUEST_CR4, &cr4);
    vmread(VM_ENTRY_CONTROLS, &entry_ctl);
    vmread(VM_EXIT_CONTROLS, &exit_ctl);
    vmread(PIN_BASED_VM_EXEC_CONTROL, &pin_ctl);
    vmread(CPU_BASED_VM_EXEC_CONTROL, &cpu_ctl);
    vmread(SECONDARY_VM_EXEC_CONTROL, &cpu2_ctl);
    vmread(GUEST_CS_SELECTOR, &cs_sel);
    vmread(GUEST_CS_BASE, &cs_base);
    vmread(GUEST_CS_LIMIT, &cs_limit);
    vmread(GUEST_CS_AR_BYTES, &cs_ar);
    vmread(GUEST_SS_SELECTOR, &ss_sel);
    vmread(GUEST_SS_BASE, &ss_base);
    vmread(GUEST_SS_LIMIT, &ss_limit);
    vmread(GUEST_SS_AR_BYTES, &ss_ar);
    vmread(GUEST_TR_SELECTOR, &tr_sel);
    vmread(GUEST_TR_BASE, &tr_base);
    vmread(GUEST_TR_LIMIT, &tr_limit);
    vmread(GUEST_TR_AR_BYTES, &tr_ar);
    vmread(GUEST_LDTR_SELECTOR, &ldtr_sel);
    vmread(GUEST_LDTR_BASE, &ldtr_base);
    vmread(GUEST_LDTR_LIMIT, &ldtr_limit);
    vmread(GUEST_LDTR_AR_BYTES, &ldtr_ar);
    vmread(GUEST_DS_SELECTOR, &ds_sel);
    vmread(GUEST_DS_AR_BYTES, &ds_ar);
    vmread(GUEST_ES_SELECTOR, &es_sel);
    vmread(GUEST_ES_AR_BYTES, &es_ar);
    vmread(GUEST_FS_SELECTOR, &fs_sel);
    vmread(GUEST_FS_AR_BYTES, &fs_ar);
    vmread(GUEST_GS_SELECTOR, &gs_sel);
    vmread(GUEST_GS_AR_BYTES, &gs_ar);
    vmread(HOST_CR0, &host_cr0);
    vmread(HOST_CR4, &host_cr4);
    vmread(HOST_RIP, &host_rip);
    vmread(HOST_RSP, &host_rsp);
    vmread(HOST_IA32_EFER, &host_efer);
    vmread(HOST_CS_SELECTOR, &host_cs);
    vmread(GUEST_IA32_PAT, &g_pat);
    vmread(GUEST_RFLAGS, &g_rflags);
    vmread(GUEST_DR7, &g_dr7);
    vmread(VMCS_LINK_POINTER, &vmcs_link);
    rdmsrl(MSR_IA32_VMX_CR0_FIXED0, cr0_fixed0);
    rdmsrl(MSR_IA32_VMX_CR0_FIXED1, cr0_fixed1);
    rdmsrl(MSR_IA32_VMX_CR4_FIXED0, cr4_fixed0);
    rdmsrl(MSR_IA32_VMX_CR4_FIXED1, cr4_fixed1);
    printk(KERN_INFO "[*] Hyperion: VMCS diag CPU%d — "
                     "G_EFER=0x%llx G_CR0=0x%llx G_CR4=0x%llx "
                     "G_PAT=0x%llx G_RFLAGS=0x%llx G_DR7=0x%llx\n",
           ProcessorID, efer, cr0, cr4, g_pat, g_rflags, g_dr7);
    printk(KERN_INFO "[*] Hyperion: VMCS diag CPU%d — "
                     "G_CS(sel=%llx ar=%llx) G_SS(sel=%llx ar=%llx)\n",
           ProcessorID, cs_sel, cs_ar, ss_sel, ss_ar);
    printk(KERN_INFO "[*] Hyperion: VMCS diag CPU%d — "
                     "G_TR(sel=%llx base=%llx lim=%llx ar=%llx)\n",
           ProcessorID, tr_sel, tr_base, tr_limit, tr_ar);
    printk(KERN_INFO "[*] Hyperion: VMCS diag CPU%d — "
                     "G_LDTR(sel=%llx ar=%llx) G_DS(sel=%llx ar=%llx) "
                     "G_ES(sel=%llx ar=%llx)\n",
           ProcessorID, ldtr_sel, ldtr_ar, ds_sel, ds_ar, es_sel, es_ar);
    printk(KERN_INFO "[*] Hyperion: VMCS diag CPU%d — "
                     "G_FS(sel=%llx ar=%llx) G_GS(sel=%llx ar=%llx) "
                     "VMCS_LINK=0x%llx\n",
           ProcessorID, fs_sel, fs_ar, gs_sel, gs_ar, vmcs_link);
    printk(KERN_INFO "[*] Hyperion: VMCS diag CPU%d — "
                     "H_CR0=0x%llx H_CR4=0x%llx H_EFER=0x%llx "
                     "H_RIP=0x%llx H_CS=%llx\n",
           ProcessorID, host_cr0, host_cr4, host_efer,
           host_rip, host_cs);
    printk(KERN_INFO "[*] Hyperion: VMCS diag CPU%d — "
                     "VENTRY=0x%llx VEXIT=0x%llx PIN=0x%llx "
                     "CPU=0x%llx CPU2=0x%llx\n",
           ProcessorID, entry_ctl, exit_ctl, pin_ctl,
           cpu_ctl, cpu2_ctl);
    printk(KERN_INFO "[*] Hyperion: VMCS diag CPU%d — "
                     "CR0_FIXED0=0x%llx CR0_FIXED1=0x%llx "
                     "CR4_FIXED0=0x%llx CR4_FIXED1=0x%llx\n",
           ProcessorID, cr0_fixed0, cr0_fixed1,
           cr4_fixed0, cr4_fixed1);
  }

  printk(KERN_INFO "[*] Hyperion: Executing VMLAUNCH on CPU%d.\n", ProcessorID);

  // Execute VMLAUNCH
  {
    uint8_t status = 0;
    __asm__ volatile("vmlaunch\n\t"
                     "setna %0\n\t"
                     : "=qm"(status)
                     :
                     : "cc", "memory");

    if (status) {
      uint64_t error_code = 0;
      vmread(VM_INSTRUCTION_ERROR, &error_code);
      __asm__ volatile("vmxoff\n\t" ::: "cc");
      printk(KERN_ERR "[*] Hyperion: VMLAUNCH error: 0x%llx (CPU%d)\n",
             error_code, ProcessorID);
    } else {
      printk(KERN_INFO "[*] Hyperion: VMLAUNCH succeeded on CPU%d\n",
             ProcessorID);
    }
  }
}

/*
 * SetupVmcsAndVirtualizeMachine
 *
 * Configures the VMCS so that the guest state mirrors the current host
 * state.  GUEST_RIP is set to VmxRestoreState, and GUEST_RSP is set to
 * the stack pointer we captured before the launch.
 */
static void
SetupVmcsAndVirtualizeMachine(struct virtual_machine_state *guest_state,
                              uint64_t EPTP, void *GuestStack) {
  setup_vmcs(guest_state, EPTP);

  vmwrite(GUEST_RIP, (uint64_t)VmxRestoreState);
  vmwrite(GUEST_RSP, (uint64_t)GuestStack);
}
