#include <linux/linkage.h>
#include <asm/nospec-branch.h>

# ---------------------------------------------------------------------------
# VmxSaveState
# Saves all general-purpose registers, passes the current RSP as the guest
# stack argument, and calls VirtualizeCurrentSystem (our C setup function).
#
# Calling convention (System V AMD64 ABI):
#   %rdi = ProcessorID, %rsi = EPTP, %rdx = GuestStack
# ---------------------------------------------------------------------------
.globl VmxSaveState
VmxSaveState:
    push    %rax
    push    %rcx
    push    %rdx
    push    %rbx
    push    %rbp
    push    %rsi
    push    %rdi
    push    %r8
    push    %r9
    push    %r10
    push    %r11
    push    %r12
    push    %r13
    push    %r14
    push    %r15

    sub     $0x20, %rsp

    mov     %rsp, %rdx

    call    VirtualizeCurrentSystem

    add     $0x20, %rsp

    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %r11
    pop     %r10
    pop     %r9
    pop     %r8
    pop     %rdi
    pop     %rsi
    pop     %rbp
    pop     %rbx
    pop     %rdx
    pop     %rcx
    pop     %rax

    RET


# ---------------------------------------------------------------------------
# VmxRestoreState
# Set as GUEST_RIP.  Restores all registers saved by VmxSaveState
# and returns to the original kernel execution path.
# ---------------------------------------------------------------------------
.globl VmxRestoreState
VmxRestoreState:
    add     $0x20, %rsp

    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %r11
    pop     %r10
    pop     %r9
    pop     %r8
    pop     %rdi
    pop     %rsi
    pop     %rbp
    pop     %rbx
    pop     %rdx
    pop     %rcx
    pop     %rax

    RET

.globl asm_vmxoff_and_restore_state
.globl g_stack_pointer_for_returning
.globl g_base_pointer_for_returning

asm_vmxoff_and_restore_state:
    /* Turn off VMX */
    vmxoff

    /* Restore the stack and frame pointers saved before VMLAUNCH */
    mov     g_stack_pointer_for_returning(%rip), %rsp
    mov     g_base_pointer_for_returning(%rip), %rbp

    /*
     * The return address was pushed onto the stack when launch_vm() was
     * called.  Since we restored the old RSP, the return address is at
     * [RSP], so a normal RET will jump back to the caller.
     */
    addq    $8, %rsp       /* skip the saved frame */
    RET

.extern main_vmexit_handler
.extern vm_resume_instruction
.extern g_GuestRIP
.extern g_GuestRSP

.globl asm_vmexit_handler
asm_vmexit_handler:
    /* Save all GPRs in System V ABI-prescribed order */
    push    %r15
    push    %r14
    push    %r13
    push    %r12
    push    %r11
    push    %r10
    push    %r9
    push    %r8
    push    %rdi
    push    %rsi
    push    %rbp
    push    %rbp           /* RSP (input for handler) — push twice for alignment */
    push    %rbx
    push    %rdx
    push    %rcx
    push    %rax

    mov     %rsp, %rdi     /* arg0 = pointer to saved guest regs */
    sub     $0x28, %rsp    /* shadow space + stack alignment for call */

    call    main_vmexit_handler

    add     $0x28, %rsp

    cmp     $1, %al
    je      VmxoffHandler

    pop     %rax
    pop     %rcx
    pop     %rdx
    pop     %rbx
    pop     %rbp           /* discard extra RBP */
    pop     %rbp
    pop     %rsi
    pop     %rdi
    pop     %r8
    pop     %r9
    pop     %r10
    pop     %r11
    pop     %r12
    pop     %r13
    pop     %r14
    pop     %r15

    sub     $0x0100, %rsp  /* safety margin for future functions */

    jmp     vm_resume_instruction

VmxoffHandler:
    vmxoff

    pop     %rax
    pop     %rcx
    pop     %rdx
    pop     %rbx
    pop     %rbp           /* discard extra RBP */
    pop     %rbp
    pop     %rsi
    pop     %rdi
    pop     %r8
    pop     %r9
    pop     %r10
    pop     %r11
    pop     %r12
    pop     %r13
    pop     %r14
    pop     %r15

    mov     g_GuestRSP(%rip), %rsp
    ANNOTATE_RETPOLINE_SAFE
    jmp     *g_GuestRIP(%rip)
