# Stack Peek Forwarding Test
#
# Exercises the static SRP path for stack peeks (loads from %rsp that are
# not pop/ret macro-ops). A push is followed by a mov (%rsp) peek.
#
# Return 0 on success, 1 on correctness failure.

.global _start

.text
_start:
    mov $150, %rcx

loop_start:
    # Use rcx as the pushed value so each iteration is distinct
    pushq %rcx

    # Stack peek: read top of stack without popping
    movq (%rsp), %rax
    cmp %rcx, %rax
    jne fail

    popq %rax

    dec %rcx
    jnz loop_start

    xor %rdi, %rdi
    mov $60, %rax
    syscall

fail:
    mov $1, %rdi
    mov $60, %rax
    syscall
