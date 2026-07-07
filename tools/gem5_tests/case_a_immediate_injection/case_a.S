.global _start
_start:
    mov $100, %rcx
loop_start:
    movq %rax, (%rsp)
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    movq (%rsp), %rbx
    addq $1, %rbx
    dec %rcx
    jnz loop_start

    # Exit syscall
    mov $60, %rax
    xor %rdi, %rdi
    syscall
