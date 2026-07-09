#Immediate Injection
# Covers the case where a store should have enough time to fully complete before
# forwarding and be eligible for `Immediate Injection` into the register file.
#
# Return 0 on success, 1 on correctness failure.

.global _start
_start:

    #Value for expected for ST/LDs
    mov $127, %r8

    # Do 100 Iterations, ensuring that the MRP has time to warm up
    mov $100, %rcx

loop_start:

    #Change the value of r8 in each iteration and store it
    addq $2, %r8
    movq %r8, (%rsp)

    #Wait for 50 loop iterations to ensure the store completes
    mov $50, %r9

wait_loop:
    nop
    dec %r9
    jnz wait_loop

    #Load the value and check that it matches what was stored
    movq (%rsp), %r10
    cmp %r8, %r10
    jne fail

    #Equivlent to `if ((--rcx) != 0 ) goto loop_start;`
    dec %rcx
    jne loop_start

    #End of loop with no errors, return 0
    xor %rdi, %rdi
    jmp exit

fail:
    #Correctness error in load, return 1 for failure
    mov $1, %rdi
    jmp exit

exit:
    mov $60, %rax
    syscall

