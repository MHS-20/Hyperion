#include "hyperion.h"
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/smp.h>

struct virtual_machine_state *g_guest_state = NULL;
int processor_count = 0;

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
