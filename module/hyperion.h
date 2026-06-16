#include <linux/ioctl.h>
#include <linux/types.h>

/* 'k' is our magic number */
#define HYPERION_MAGIC 'k'

/* Initialize VMX on all logical processors */
#define IOCTL_INIT_VMX _IO(HYPERION_MAGIC, 0)

/* Terminate VMX on all logical processors */
#define IOCTL_TERMINATE_VMX _IO(HYPERION_MAGIC, 1)

/* Example: send a buffer to the kernel */
#define IOCTL_SEND_BUFFER _IOW(HYPERION_MAGIC, 2, unsigned long)

struct virtual_machine_state {
  uint64_t vmxon_region; /* Physical address of VMXON region */
  uint64_t vmcs_region;  /* Physical address of VMCS region  */
};

extern struct virtual_machine_state *g_guest_state;
extern int processor_count;
