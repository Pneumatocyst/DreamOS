# void switch_to(uint64_t *save_rsp, uint64_t load_rsp)
#
# The whole trick of multitasking, in 16 instructions. Push the registers a C
# function must preserve onto the current task's stack, remember where that
# stack ends, jump to the other task's stack, pop *its* registers, and return.
# That `ret` returns into the other task, right where it last called switch_to.

.text
.global switch_to
switch_to:
    pushfq
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)      # save: where this task's stack is
    movq %rsi, %rsp        # load: the other task's stack
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    popfq
    ret

.section .note.GNU-stack,"",@progbits
