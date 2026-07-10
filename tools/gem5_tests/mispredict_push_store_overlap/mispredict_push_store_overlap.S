# Push-Store Overlap
# Covers the case where a push - pop pair fails to forward due to a store operation
# There should be few predictions as the pop's should freeze due to mismatches.
#
# This differs from the no_forwarding case, as the Static path assumes stacks are
# good unless an entry in the MRP says otherwise.
#
# Return 0 on success, 1 on correctness failure.

.global _start

.text
_start:

    # Use rcx as loop counter, compared against 100 below
    movq $0, %rcx

loop_start:
    # Use rcx as the pushed value so each iteration is distinct
    pushq %rcx
    movq %rcx, %r8

    # When the loop count is even, do not store over the push
    testq $1, %rcx
    je no_store
    #Create a different value from rcx to store
    addq %r8, %r8
    addq %r8, %r8
    addq %r8, %r8
    #Store over the push some of the time
    movq %r8, (%rsp)
no_store:

    #Check stack value
    popq %rax
    cmp %r8, %rax
    jne fail

    #Equivlent to `if ((++rcx) != 100 ) goto loop_start;`
    incq %rcx
    cmp $100, %rcx
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
