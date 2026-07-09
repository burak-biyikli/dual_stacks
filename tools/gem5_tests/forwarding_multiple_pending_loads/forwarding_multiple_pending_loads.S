# Multiple Pending Loads on a Single Store
#
# Two loads read the same address while the producing store's source
# register is still pending. Both loads should wake on the same writeback.
#
# Return 0 on success, 1 on correctness failure.

.global _start

.section .bss
    .lcomm mem_slot, 8

.text
_start:
    lea mem_slot(%rip), %r8
    mov $150, %rcx
    xor %rdx, %rdx

loop_start:
    add %rcx, %rdx
    mov %rdx, (%r8)

    mov (%r8), %rax
    mov (%r8), %rbx

    cmp %rdx, %rax
    jne fail
    cmp %rdx, %rbx
    jne fail

    dec %rcx
    jnz loop_start

    xor %rdi, %rdi
    mov $60, %rax
    syscall

fail:
    mov $1, %rdi
    mov $60, %rax
    syscall
