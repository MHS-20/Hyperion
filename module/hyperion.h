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
