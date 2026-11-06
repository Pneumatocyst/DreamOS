# void enter_user(uint64_t rip, uint64_t rsp, uint64_t argc, uint64_t argv)
#
# The one-way door into ring 3. `iretq` normally returns from an interrupt by
# popping RIP, CS, RFLAGS, RSP and SS. We build that frame by hand with *user*
# code and stack segments (the low 2 bits = 3 = ring 3), so "returning" drops the
# CPU into user mode at the program's entry point. The only way back in is an
# interrupt: the timer, a fault, or `int 0x80`.

.text
.global enter_user
enter_user:
    pushq $0x1b            # SS: user data segment (0x18) | ring 3
    pushq %rsi             # RSP: the program's stack
    pushq $0x202           # RFLAGS: interrupts on
    pushq $0x23            # CS: user code segment (0x20) | ring 3
    pushq %rdi             # RIP: the program's entry point
    movq %rdx, %rdi        # main's argc...
    movq %rcx, %rsi        # ...and argv
    xorl %eax, %eax        # don't leak kernel values to the program
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq

.section .note.GNU-stack,"",@progbits
