#include "ept.h"
#include <linux/ioctl.h>
#ifdef __KERNEL__
#include <linux/types.h>

void enable_vmx_operation(void);
void *physical_to_virtual(uint64_t pa);
uint64_t virtual_to_physical(void *va);
#else
#include <stdint.h>
#endif

/* 'k' is our magic number */
#define HYPERION_MAGIC 'k'

/* Initialize VMX on all logical processors */
#define IOCTL_INIT_VMX _IO(HYPERION_MAGIC, 0)

/* Terminate VMX on all logical processors */
#define IOCTL_TERMINATE_VMX _IO(HYPERION_MAGIC, 1)

/* Example: send a buffer to the kernel */
#define IOCTL_SEND_BUFFER _IOW(HYPERION_MAGIC, 2, unsigned long)

struct virtual_machine_state {
  /* Physical addresses of the VMXON and VMCS regions */
  uint64_t vmxon_region;
  uint64_t vmcs_region;

  /* Extended Page Table Pointer (full 64-bit EPTP value) */
  uint64_t eptp;

  /* Separate stack for the VMM (VM-Exit handler) */
  uint64_t vmm_stack;

  /* MSR Bitmap — virtual and physical addresses */
  uint64_t msr_bitmap;
  uint64_t msr_bitmap_physical;

#ifdef __KERNEL__
  void *vmxon_alloc;     /* original kmalloc ptr for vmxon_region (for kfree) */
  void *vmcs_alloc;      /* original kmalloc ptr for vmcs_region  (for kfree) */
  void *vmm_stack_virt;  /* virtual address of VMM stack (for kfree) */
  void *msr_bitmap_virt; /* virtual address of MSR Bitmap (for kfree) */
#endif
};

extern struct virtual_machine_state *g_guest_state;
extern int processor_count;

bool is_vmx_supported(void);
bool initialize_vmx(void);
void terminate_vmx(void);

bool allocate_vmxon_region(struct virtual_machine_state *state);
bool allocate_vmcs_region(struct virtual_machine_state *state);

uint64_t initialize_eptp(void);

#define CPU_BASED_VIRTUAL_INTR_PENDING 0x00000004
#define CPU_BASED_USE_TSC_OFFSETING 0x00000008
#define CPU_BASED_HLT_EXITING 0x00000080
#define CPU_BASED_INVLPG_EXITING 0x00000200
#define CPU_BASED_MWAIT_EXITING 0x00000400
#define CPU_BASED_RDPMC_EXITING 0x00000800
#define CPU_BASED_RDTSC_EXITING 0x00001000
#define CPU_BASED_CR3_LOAD_EXITING 0x00008000
#define CPU_BASED_CR3_STORE_EXITING 0x00010000
#define CPU_BASED_CR8_LOAD_EXITING 0x00080000
#define CPU_BASED_CR8_STORE_EXITING 0x00100000
#define CPU_BASED_TPR_SHADOW 0x00200000
#define CPU_BASED_VIRTUAL_NMI_PENDING 0x00400000
#define CPU_BASED_MOV_DR_EXITING 0x00800000
#define CPU_BASED_UNCOND_IO_EXITING 0x01000000
#define CPU_BASED_ACTIVATE_IO_BITMAP 0x02000000
#define CPU_BASED_MONITOR_TRAP_FLAG 0x08000000
#define CPU_BASED_ACTIVATE_MSR_BITMAP 0x10000000
#define CPU_BASED_MONITOR_EXITING 0x20000000
#define CPU_BASED_PAUSE_EXITING 0x40000000
#define CPU_BASED_ACTIVATE_SECONDARY_CONTROLS 0x80000000

#define CPU_BASED_CTL2_ENABLE_EPT 0x00000002
#define CPU_BASED_CTL2_RDTSCP 0x00000008
#define CPU_BASED_CTL2_ENABLE_VPID 0x00000020
#define CPU_BASED_CTL2_UNRESTRICTED_GUEST 0x00000080
#define CPU_BASED_CTL2_ENABLE_VMFUNC 0x00002000

#define VM_ENTRY_IA32E_MODE 0x00000200
#define VM_ENTRY_SMM 0x00000400
#define VM_ENTRY_DEACT_DUAL_MONITOR 0x00000800
#define VM_ENTRY_LOAD_GUEST_PAT 0x00004000

#define PIN_BASED_EXT_INTR_EXITING 0x00000001
#define PIN_BASED_NMI_EXITING 0x00000008
#define PIN_BASED_VIRTUAL_NMI 0x00000020
#define PIN_BASED_ACTIVE_VMX_TIMER 0x00000040
#define PIN_BASED_PROCESS_POSTED_INTERRUPTS 0x00000080
