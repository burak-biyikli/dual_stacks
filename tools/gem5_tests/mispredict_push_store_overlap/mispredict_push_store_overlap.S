# Push-Store Overlap
# Covers the case where a push - pop pair fails to forward due to a store operation
# There should be few predictions as the pop's should freeze due to mismatches.
#
# This differs from the no_forwarding case, as the Static path assumes stacks are
# good unless an entry in the MRP says otherwise.
#
# Return 0 on success,
# 1 on correctness failure on case 1 (store to stack)
# 2 on correctness failure in case 2 (aliased store to stack)

.global _start

.text
_start:

    # Use rcx as loop counter, compared against 100 below
    movq $0, %rcx

case1_loop_start:
    # Use rcx as the pushed value so each iteration is distinct
    pushq %rcx
    movq %rcx, %r8

    # When the loop count is even, do not store over the push
    testq $1, %rcx
    je no_store_1
    # Create a different value from rcx to store, use multiple instructions
    # to induce some additional latency on the data side
    addq %r8, %r8
    addq %r8, %r8
    addq %r8, %r8
    # Store over the push some of the time
    # Note that r8 always holds the correct expected value
    movq %r8, (%rsp)
no_store_1:

    # Check stack value
    popq %rax
    cmp %r8, %rax
    jne fail_1

    # Equivalent to `if ((++rcx) != 100 ) goto case1_loop_start;`
    incq %rcx
    cmp $100, %rcx
    jne case1_loop_start

start_case2:

    # Reset rcx as loop counter, compared against 100 below
    movq $0, %rcx

case2_loop_start:
    # Use rcx as the pushed value so each iteration is distinct
    pushq %rcx
    movq %rcx, %r8

    # When the loop count is even, do not store over the push
    testq $1, %rcx
    je no_store_2
    # Create a different value from rcx to store
    addq %r8, %r8
    addq %r8, %r8
    addq %r8, %r8
    # Store over the push some of the time while hiding the fact
    # that we are modifying the stack and changing the timing of LSU
    # seeing that there is an overlap
    movq %rsp, %r9
    xorq %r8, %r9
    xorq %r8, %r9
    movq %r8, (%r9)
no_store_2:

    # Check stack value
    popq %rax
    cmp %r8, %rax
    jne fail_2

    # Equivalent to `if ((++rcx) != 100 ) goto case2_loop_start;`
    incq %rcx
    cmp $100, %rcx
    jne case2_loop_start

    # End of both cases with no errors, return 0
    xor %rdi, %rdi
    jmp exit

fail_1:
    # Correctness error in load, return 1 for failure
    mov $1, %rdi
    jmp exit

fail_2:
    # Correctness error in load, return 2 for failure
    mov $2, %rdi
    jmp exit

exit:
    mov $60, %rax
    syscall
