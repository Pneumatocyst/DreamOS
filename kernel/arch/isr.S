# Interrupt entry stubs. The CPU jumps here on exceptions (0-31) and
# hardware IRQs (32-47). Each stub makes the stack look the same — an error
# code and a vector number — then isr_common saves every register and calls C.

.macro ISR_NOERR n
isr\n:
    pushq $0            # fake error code so every frame has the same shape
    pushq $\n
    jmp isr_common
.endm

.macro ISR_ERR n
isr\n:
    pushq $\n           # CPU already pushed a real error code
    jmp isr_common
.endm

.text
.irp n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
    ISR_NOERR \n
.endr
.irp n, 8,10,11,12,13,14,17,21,29,30
    ISR_ERR \n
.endr
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    ISR_NOERR \n
.endr

# int 0x80: the system call gate
.global isr128
    ISR_NOERR 128

isr_common:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi     # arg 1: struct regs *
    call isr_dispatch
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp      # drop vector + error code
    iretq

# Table of stub addresses, indexed by vector, read by idt.c
.section .rodata
.global isr_table
isr_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .quad isr\n
.endr

.section .note.GNU-stack,"",@progbits
