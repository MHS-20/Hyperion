#include "ept.h"
#include <linux/ioctl.h>
#ifdef __KERNEL__
#include <linux/types.h>

void enable_vmx_operation(void);
void *physical_to_virtual(uint64_t pa);
uint64_t virtual_to_physical(void *va);
#else
#include <stdbool.h>
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

#define IOCTL_READ_LOG_BUFFER _IOR(HYPERION_MAGIC, 3, unsigned long)

typedef struct {
  bool     is_vmxoff_executed;
  uint64_t guest_rip;
  uint64_t guest_rsp;
} VMX_VMXOFF_STATE;

/* VMCALL service numbers — used to request services from VMX root mode */
#define VMCALL_TEST                    0x1
#define VMCALL_VMXOFF                  0x2
#define VMCALL_EXEC_HOOK_PAGE          0x3
#define VMCALL_INVEPT_ALL_CONTEXT      0x4
#define VMCALL_INVEPT_SINGLE_CONTEXT   0x5

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
  void *pre_allocated_buffer; /* pre-allocated buffer for VMX root mode EPT splitting */
  bool increment_rip;    /* if false, don't advance GUEST_RIP on VMRESUME */
  VMX_VMXOFF_STATE vmxoff_state; /* per-core VMXOFF return state */
#endif
};

extern struct virtual_machine_state *g_guest_state;
extern int processor_count;

typedef struct _GUEST_REGS {
  uint64_t rax;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rbx;
  uint64_t rsp;
  uint64_t rbp;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
} GUEST_REGS, *PGUEST_REGS;

extern void VmxSaveState(void);
extern void VmxRestoreState(void);
extern void VirtualizeCurrentSystem(int ProcessorID, uint64_t EPTP,
                                    void *GuestStack);
extern uint64_t AsmVmxVmcall(uint64_t VmcallNumber, uint64_t OptionalParam1,
                             uint64_t OptionalParam2, uint64_t OptionalParam3);

bool is_vmx_supported(void);
bool initialize_vmx(void);
void terminate_vmx(void);
void HvTerminateVmx(void);
void VmxVmxoff(void);
uint64_t HvReturnStackPointerForVmxoff(void);
uint64_t HvReturnInstructionPointerForVmxoff(void);

bool allocate_vmxon_region(struct virtual_machine_state *state);
bool allocate_vmcs_region(struct virtual_machine_state *state);
bool clear_vmcs_state(struct virtual_machine_state *guest_state);

uint64_t initialize_eptp(void);
bool EptLogicalProcessorInitialize(void);
void HvSetMonitorTrapFlag(bool Enable);
void EptHandleMonitorTrapFlag(void);
bool EptHandleHookedPage(uint64_t PhysicalAddress, uint64_t GuestRip,
                         bool IsReadViolation, bool IsWriteViolation,
                         bool IsExecuteViolation);
bool EptPerformPageHook(void *TargetFunction, void *HookFunction,
                        void **OrigFunction, bool HookRead,
                        bool HookWrite, bool HookExecute);
bool EptPageUnHookSinglePage(void *TargetFunction);
#ifdef __KERNEL__
int LogReadBuffer(void __user *UserBuffer, uint32_t UserBufferSize,
                  uint32_t *BytesWritten);
#endif
bool EptPageHook(void *TargetFunc, bool HasLaunched);
#ifdef __KERNEL__
EPT_PML2_ENTRY *EptGetPml2Entry(VMM_EPT_PAGE_TABLE *EptPageTable,
                                 uint64_t PhysicalAddress);
EPT_PML1_ENTRY *EptGetPml1Entry(VMM_EPT_PAGE_TABLE *EptPageTable,
                                 uint64_t PhysicalAddress);
bool EptSplitLargePage(VMM_EPT_PAGE_TABLE *EptPageTable,
                       void *PreAllocatedBuffer,
                       uint64_t PhysicalAddress, int CoreIndex);
#endif
bool EptVmxRootModePageHook(void *TargetFunc, bool HasLaunched);
bool EptHandleEptViolation(uint64_t ExitQualification, uint64_t GuestPhysicalAddr);
uint8_t Invept(uint32_t Type, INVEPT_DESCRIPTOR *Descriptor);
uint8_t InveptAllContexts(void);
uint8_t InveptSingleContext(uint64_t EptPointer);
uint8_t InvvpidSingleContext(uint16_t VPID);
void HvNotifyAllToInvalidateEpt(void);

uint64_t vmptrst_instruction(void);
uint8_t main_vmexit_handler(uint64_t *guest_regs);
void vm_resume_instruction(void);

enum vmcs_fields {
  VIRTUAL_PROCESSOR_ID = 0x00000000,
  GUEST_ES_SELECTOR = 0x00000800,
  GUEST_CS_SELECTOR = 0x00000802,
  GUEST_SS_SELECTOR = 0x00000804,
  GUEST_DS_SELECTOR = 0x00000806,
  GUEST_FS_SELECTOR = 0x00000808,
  GUEST_GS_SELECTOR = 0x0000080a,
  GUEST_LDTR_SELECTOR = 0x0000080c,
  GUEST_TR_SELECTOR = 0x0000080e,
  HOST_ES_SELECTOR = 0x00000c00,
  HOST_CS_SELECTOR = 0x00000c02,
  HOST_SS_SELECTOR = 0x00000c04,
  HOST_DS_SELECTOR = 0x00000c06,
  HOST_FS_SELECTOR = 0x00000c08,
  HOST_GS_SELECTOR = 0x00000c0a,
  HOST_TR_SELECTOR = 0x00000c0c,
  IO_BITMAP_A = 0x00002000,
  IO_BITMAP_A_HIGH = 0x00002001,
  IO_BITMAP_B = 0x00002002,
  IO_BITMAP_B_HIGH = 0x00002003,
  MSR_BITMAP = 0x00002004,
  MSR_BITMAP_HIGH = 0x00002005,
  VM_EXIT_MSR_STORE_ADDR = 0x00002006,
  VM_EXIT_MSR_STORE_ADDR_HIGH = 0x00002007,
  VM_EXIT_MSR_LOAD_ADDR = 0x00002008,
  VM_EXIT_MSR_LOAD_ADDR_HIGH = 0x00002009,
  VM_ENTRY_MSR_LOAD_ADDR = 0x0000200a,
  VM_ENTRY_MSR_LOAD_ADDR_HIGH = 0x0000200b,
  TSC_OFFSET = 0x00002010,
  TSC_OFFSET_HIGH = 0x00002011,
  VIRTUAL_APIC_PAGE_ADDR = 0x00002012,
  VIRTUAL_APIC_PAGE_ADDR_HIGH = 0x00002013,
  EPT_POINTER = 0x0000201A,
  EPT_POINTER_HIGH = 0x0000201B,
  EPTP_LIST = 0x00002024,
  EPTP_LIST_HIGH = 0x00002025,
  GUEST_PHYSICAL_ADDRESS = 0x00002400,
  GUEST_PHYSICAL_ADDRESS_HIGH = 0x00002401,
  VMCS_LINK_POINTER = 0x00002800,
  VMCS_LINK_POINTER_HIGH = 0x00002801,
  GUEST_IA32_DEBUGCTL = 0x00002802,
  GUEST_IA32_DEBUGCTL_HIGH = 0x00002803,
  PIN_BASED_VM_EXEC_CONTROL = 0x00004000,
  CPU_BASED_VM_EXEC_CONTROL = 0x00004002,
  EXCEPTION_BITMAP = 0x00004004,
  PAGE_FAULT_ERROR_CODE_MASK = 0x00004006,
  PAGE_FAULT_ERROR_CODE_MATCH = 0x00004008,
  CR3_TARGET_COUNT = 0x0000400a,
  VM_EXIT_CONTROLS = 0x0000400c,
  VM_EXIT_MSR_STORE_COUNT = 0x0000400e,
  VM_EXIT_MSR_LOAD_COUNT = 0x00004010,
  VM_ENTRY_CONTROLS = 0x00004012,
  VM_ENTRY_MSR_LOAD_COUNT = 0x00004014,
  VM_ENTRY_INTR_INFO_FIELD = 0x00004016,
  VM_ENTRY_EXCEPTION_ERROR_CODE = 0x00004018,
  VM_ENTRY_INSTRUCTION_LEN = 0x0000401a,
  TPR_THRESHOLD = 0x0000401c,
  SECONDARY_VM_EXEC_CONTROL = 0x0000401e,
  VM_INSTRUCTION_ERROR = 0x00004400,
  VM_EXIT_REASON = 0x00004402,
  VM_EXIT_INTR_INFO = 0x00004404,
  VM_EXIT_INTR_ERROR_CODE = 0x00004406,
  IDT_VECTORING_INFO_FIELD = 0x00004408,
  IDT_VECTORING_ERROR_CODE = 0x0000440a,
  VM_EXIT_INSTRUCTION_LEN = 0x0000440c,
  VMX_INSTRUCTION_INFO = 0x0000440e,
  GUEST_ES_LIMIT = 0x00004800,
  GUEST_CS_LIMIT = 0x00004802,
  GUEST_SS_LIMIT = 0x00004804,
  GUEST_DS_LIMIT = 0x00004806,
  GUEST_FS_LIMIT = 0x00004808,
  GUEST_GS_LIMIT = 0x0000480a,
  GUEST_LDTR_LIMIT = 0x0000480c,
  GUEST_TR_LIMIT = 0x0000480e,
  GUEST_GDTR_LIMIT = 0x00004810,
  GUEST_IDTR_LIMIT = 0x00004812,
  GUEST_ES_AR_BYTES = 0x00004814,
  GUEST_CS_AR_BYTES = 0x00004816,
  GUEST_SS_AR_BYTES = 0x00004818,
  GUEST_DS_AR_BYTES = 0x0000481a,
  GUEST_FS_AR_BYTES = 0x0000481c,
  GUEST_GS_AR_BYTES = 0x0000481e,
  GUEST_LDTR_AR_BYTES = 0x00004820,
  GUEST_TR_AR_BYTES = 0x00004822,
  GUEST_INTERRUPTIBILITY_INFO = 0x00004824,
  GUEST_ACTIVITY_STATE = 0x00004826,
  GUEST_SMBASE = 0x00004828,
  GUEST_SYSENTER_CS = 0x0000482A,
  GUEST_IA32_PAT = 0x00002804,
  GUEST_IA32_PAT_HIGH = 0x00002805,
  GUEST_IA32_EFER = 0x00002806,
  HOST_IA32_PAT = 0x00002C00,
  HOST_IA32_PAT_HIGH = 0x00002C01,
  HOST_IA32_SYSENTER_CS = 0x00004c00,
  HOST_IA32_EFER = 0x00002c02,
  CR0_GUEST_HOST_MASK = 0x00006000,
  CR4_GUEST_HOST_MASK = 0x00006002,
  CR0_READ_SHADOW = 0x00006004,
  CR4_READ_SHADOW = 0x00006006,
  CR3_TARGET_VALUE0 = 0x00006008,
  CR3_TARGET_VALUE1 = 0x0000600a,
  CR3_TARGET_VALUE2 = 0x0000600c,
  CR3_TARGET_VALUE3 = 0x0000600e,
  EXIT_QUALIFICATION = 0x00006400,
  GUEST_LINEAR_ADDRESS = 0x0000640a,
  GUEST_CR0 = 0x00006800,
  GUEST_CR3 = 0x00006802,
  GUEST_CR4 = 0x00006804,
  GUEST_ES_BASE = 0x00006806,
  GUEST_CS_BASE = 0x00006808,
  GUEST_SS_BASE = 0x0000680a,
  GUEST_DS_BASE = 0x0000680c,
  GUEST_FS_BASE = 0x0000680e,
  GUEST_GS_BASE = 0x00006810,
  GUEST_LDTR_BASE = 0x00006812,
  GUEST_TR_BASE = 0x00006814,
  GUEST_GDTR_BASE = 0x00006816,
  GUEST_IDTR_BASE = 0x00006818,
  GUEST_DR7 = 0x0000681a,
  GUEST_RSP = 0x0000681c,
  GUEST_RIP = 0x0000681e,
  GUEST_RFLAGS = 0x00006820,
  GUEST_PENDING_DBG_EXCEPTIONS = 0x00006822,
  GUEST_SYSENTER_ESP = 0x00006824,
  GUEST_SYSENTER_EIP = 0x00006826,
  HOST_CR0 = 0x00006c00,
  HOST_CR3 = 0x00006c02,
  HOST_CR4 = 0x00006c04,
  HOST_FS_BASE = 0x00006c06,
  HOST_GS_BASE = 0x00006c08,
  HOST_TR_BASE = 0x00006c0a,
  HOST_GDTR_BASE = 0x00006c0c,
  HOST_IDTR_BASE = 0x00006c0e,
  HOST_IA32_SYSENTER_ESP = 0x00006c10,
  HOST_IA32_SYSENTER_EIP = 0x00006c12,
  HOST_RSP = 0x00006c14,
  HOST_RIP = 0x00006c16,
};

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
#define CPU_BASED_CTL2_ENABLE_INVPCID 0x00001000
#define CPU_BASED_CTL2_ENABLE_VPID 0x00000020
#define CPU_BASED_CTL2_ENABLE_XSAVE_XRSTORS 0x00100000
#define CPU_BASED_CTL2_UNRESTRICTED_GUEST 0x00000080
#define CPU_BASED_CTL2_ENABLE_VMFUNC 0x00002000

#define VM_ENTRY_IA32E_MODE 0x00000200
#define VM_EXIT_IA32E_MODE 0x00000200
#define VM_ENTRY_SMM 0x00000400
#define VM_ENTRY_DEACT_DUAL_MONITOR 0x00000800
#define VM_ENTRY_LOAD_GUEST_PAT 0x00004000

#define PIN_BASED_EXT_INTR_EXITING 0x00000001
#define PIN_BASED_NMI_EXITING 0x00000008
#define PIN_BASED_VIRTUAL_NMI 0x00000020
#define PIN_BASED_ACTIVE_VMX_TIMER 0x00000040
#define PIN_BASED_PROCESS_POSTED_INTERRUPTS 0x00000080
