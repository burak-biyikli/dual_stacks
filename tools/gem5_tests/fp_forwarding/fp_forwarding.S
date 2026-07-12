# FP Forwarding Microbenchmark
# Checks that floating-point registers correctly write back and forward via MR.
#
# Return 0 on success, 1 on correctness failure.

.global _start

.text
_start:

    # Setup 200 loop iterations
    mov $200, %rcx

loop_start:
    # Convert rcx to a double in %xmm0
    cvtsi2sd %rcx, %xmm0
    
    # Do some computations to create dependency delay
    addsd %xmm0, %xmm0
    mulsd %xmm0, %xmm0
    
    # Store xmm0 to stack
    movsd %xmm0, (%rsp)
    
    # Load from stack to xmm1
    movsd (%rsp), %xmm1
    
    # Compare xmm0 and xmm1 to make sure they are equal
    ucomisd %xmm0, %xmm1
    jp fail        # Jump if parity (unordered comparison)
    jne fail       # Jump if not equal
    
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
