/* See COPYRIGHT for copyright information. */


#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <kern/macro.h>

.code64
.text

.globl entry
entry:
    # Set up kernel stack
    leaq bootstacktop(%rip),%rsp
    xor %ebp, %ebp

    # Save LoadParams in uefi_lp.
    movq %rcx, uefi_lp(%rip)

    # Invoke C code
    call i386_init
    # Should never reach this place
    jmp .

#ifdef CONFIG_KSPACE

.global save_trapframe_trap
save_trapframe_trap:
    popq save_trapframe_ret(%rip)
    popq saved_rip(%rip)
    popq saved_cs(%rip)
    popq saved_rflags(%rip)
    popq saved_rsp(%rip)
    popq saved_ss(%rip)
    jmp save_trapframe

save_trapframe_syscall:
    popq save_trapframe_ret(%rip)
    popq saved_rip(%rip)
    movq %rsp, saved_rsp(%rip)
    pushfq
    movq $GD_KD, saved_ss(%rip)
    movq $GD_KT, saved_cs(%rip)
    popq saved_rflags(%rip)
    # fallthrough

save_trapframe:
    orl $FL_IF, saved_rflags(%rip)

    # We already pushed ss,rsp,rflags,cs,rip
    # implicitly
    leaq (bootstacktop-0x28)(%rip), %rsp

    pushq $0x0
    pushq $0x0
    pushq $0x0 # %ds
    pushq $0x0 # %es

    PUSHA

    # TODO: how do these values end up in env's trap frame?
    #   Check if they are written there.

    # Xoring ebp to not feed rubbish values to the kernel code.
    xor %ebp, %ebp
    # Stack now has trapframe on it. It's copied to rdi to make the first argument.
    movq %rsp, %rdi
    jmp *save_trapframe_ret(%rip)

.globl sys_yield
.type  sys_yield, @function
sys_yield:
    call save_trapframe_syscall
    # call select_kernel_stack # Doesn't work for some reason, breaks the code.
    call csys_yield
    jmp .

# LAB 3: Your code here:
.globl sys_exit
.type  sys_exit, @function
sys_exit:
    cli
    call save_trapframe_syscall
    call select_kernel_stack
    call csys_exit
    jmp .

#endif

select_kernel_stack:
    # We can overwrite this stack, because upon env_pop_tf
    #   all objects on the stack are considered deleted.
    # We should not create objects on stack that meant to
    #   outlive a context switch.
    # Also, we have to switch stacks, because user space
    #   stack can be both exausted and of unknown size.
    #   It's important for kernel stability.
    mov  (%rsp), %rax          # Store return address
    leaq bootstacktop(%rip),%rsp
    push %rax                  # Push return address back
    # We have some rubbish in rbp from the user space
    #   program.
    # We need to zero it out, so that
    #   entry and leave instructions would work?
    # TODO: figure this out. The first claim holds
    #   anyway, I'll leave it here.
    xor %ebp, %ebp
    ret

.data

# Allocate boot stack forcing page alignment.
.p2align PAGE_SHIFT
.globl bootstack
bootstack:
.space KERN_STACK_SIZE
.globl bootstacktop
bootstacktop:

# pagefault needs its own stack
.globl pfstack
pfstack:
.space KERN_PF_STACK_SIZE
.globl pfstacktop
pfstacktop:


# Provide storage for loader parameters.
.globl uefi_lp
uefi_lp:
.quad 0

#ifdef CONFIG_KSPACE
save_trapframe_ret:
.quad 0

.set saved_ss, bootstacktop-0x8
.set saved_rsp, bootstacktop-0x10
.set saved_rflags, bootstacktop-0x18
.set saved_cs, bootstacktop-0x20
.set saved_rip, bootstacktop-0x28
#endif
