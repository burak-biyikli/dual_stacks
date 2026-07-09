# Delayed Copy from the MRT
# Covers the case where a store does not have time to fully complete computation
# of the data value before forwarding occurs.
#
# Return 0 on success, 1 on correctness failure.

.global _start

.text
_start:

    #Setup 200 loop iterations, and zero rdx
    mov $200, %rcx
    xor %rdx, %rdx

loop_start:
    # rdx becomes ready only after these instructions execute. the sequential
    # dependency means that the data value will not exist for several cycles
    add %rcx, %rdx
    add %rdx, %rdx
    add %rdx, %rdx
    add %rdx, %rdx
    add %rdx, %rdx
    shrq $4, %rdx
    add %rdx, %rdx
    add %rdx, %rdx
    add %rdx, %rdx
    add %rdx, %rdx
    shrq $4, %rdx

    #Perform the store, where rdx likely is not ready
    mov %rdx, (%rsp)
    #Perform the corresponding store, where rdx is likely still not ready
    mov (%rsp), %rax

    #Check the load for correctness
    cmp %rdx, %rax
    jne fail

    dec %rcx
    jnz loop_start

exit:
    xor %rdi, %rdi
    mov $60, %rax
    syscall

fail:
    mov $1, %rdi
    mov $60, %rax
    syscall
