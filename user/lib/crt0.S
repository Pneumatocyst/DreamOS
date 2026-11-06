# Where every program starts. The kernel hands us argc in rdi and argv in rsi,
# which is exactly where main() expects them. When main returns, we exit.
.text
.global _start
_start:
    andq $-16, %rsp        # C wants a 16-byte aligned stack
    call main
    movl %eax, %edi
    call exit

.section .note.GNU-stack,"",@progbits
