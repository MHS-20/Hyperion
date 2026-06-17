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
    ret
