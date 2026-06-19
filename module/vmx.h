#pragma once

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* VM-exit reason codes */
#define EXIT_REASON_EXCEPTION_NMI 0
#define EXIT_REASON_EXTERNAL_INTERRUPT 1
#define EXIT_REASON_TRIPLE_FAULT 2
#define EXIT_REASON_INIT 3
#define EXIT_REASON_SIPI 4
#define EXIT_REASON_IO_SMI 5
#define EXIT_REASON_OTHER_SMI 6
#define EXIT_REASON_PENDING_VIRT_INTR 7
#define EXIT_REASON_PENDING_VIRT_NMI 8
#define EXIT_REASON_TASK_SWITCH 9
#define EXIT_REASON_CPUID 10
#define EXIT_REASON_GETSEC 11
#define EXIT_REASON_HLT 12
#define EXIT_REASON_INVD 13
#define EXIT_REASON_INVLPG 14
#define EXIT_REASON_RDPMC 15
#define EXIT_REASON_RDTSC 16
#define EXIT_REASON_RSM 17
#define EXIT_REASON_VMCALL 18
#define EXIT_REASON_VMCLEAR 19
#define EXIT_REASON_VMLAUNCH 20
#define EXIT_REASON_VMPTRLD 21
#define EXIT_REASON_VMPTRST 22
#define EXIT_REASON_VMREAD 23
#define EXIT_REASON_VMRESUME 24
#define EXIT_REASON_VMWRITE 25
#define EXIT_REASON_VMXOFF 26
#define EXIT_REASON_VMXON 27
#define EXIT_REASON_CR_ACCESS 28
#define EXIT_REASON_DR_ACCESS 29
#define EXIT_REASON_IO_INSTRUCTION 30
#define EXIT_REASON_MSR_READ 31
#define EXIT_REASON_MSR_WRITE 32
#define EXIT_REASON_INVALID_GUEST_STATE 33
#define EXIT_REASON_MSR_LOADING 34
#define EXIT_REASON_MWAIT_INSTRUCTION 36
#define EXIT_REASON_MONITOR_TRAP_FLAG 37
#define EXIT_REASON_MONITOR_INSTRUCTION 39
#define EXIT_REASON_PAUSE_INSTRUCTION 40
#define EXIT_REASON_MCE_DURING_VMENTRY 41
#define EXIT_REASON_TPR_BELOW_THRESHOLD 43
#define EXIT_REASON_APIC_ACCESS 44
#define EXIT_REASON_ACCESS_GDTR_OR_IDTR 46
#define EXIT_REASON_ACCESS_LDTR_OR_TR 47
#define EXIT_REASON_EPT_VIOLATION 48
#define EXIT_REASON_EPT_MISCONFIG 49
#define EXIT_REASON_INVEPT 50
#define EXIT_REASON_RDTSCP 51
#define EXIT_REASON_VMX_PREEMPTION_TIMER 52
#define EXIT_REASON_INVVPID 53
#define EXIT_REASON_WBINVD 54
#define EXIT_REASON_XSETBV 55
#define EXIT_REASON_APIC_WRITE 56
#define EXIT_REASON_RDRAND 57
#define EXIT_REASON_INVPCID 58
#define EXIT_REASON_RDSEED 61
#define EXIT_REASON_PML_FULL 62
#define EXIT_REASON_XSAVES 63
#define EXIT_REASON_XRSTORS 64
#define EXIT_REASON_PCOMMIT 65

#define TYPE_MOV_TO_CR   0
#define TYPE_MOV_FROM_CR 1
#define TYPE_CLTS        2
#define TYPE_LMSW        3

typedef union _VMX_EXIT_QUALIFICATION_EPT_VIOLATION {
  uint64_t all;
  struct {
    uint64_t read_access           : 1;
    uint64_t write_access          : 1;
    uint64_t execute_access        : 1;
    uint64_t ept_readable          : 1;
    uint64_t ept_writable          : 1;
    uint64_t ept_executable        : 1;
    uint64_t ept_usermode_readable : 1;
    uint64_t ept_usermode_writable : 1;
    uint64_t ept_usermode_executable: 1;
    uint64_t gva_valid             : 1;
    uint64_t reserved1             : 2;
    uint64_t nmi_unblocking        : 1;
    uint64_t reserved2             : 51;
  } fields;
} VMX_EXIT_QUALIFICATION_EPT_VIOLATION;
