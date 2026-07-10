# Demotion test
# Covers the case where a ld-st pair is sometimes broken by reads to regular non
# forwarding memory. This should cause the predictor to churn and freeze the entry
#
# Return 0 on success, 1 on correctness failure.

.global _start

#Dummy memory location for the pair of loads and stores to access
.section .bss
    .lcomm mem_array, 16

.text
_start:

    #Load the effective address of the two memory locations above into r8 and r9
    lea mem_array(%rip), %r8
    lea mem_array+8(%rip), %r9

    #Do a single store that will age out of the window into the second location
    movq $42, (%r9)

    # Use rcx as loop counter, compared against 400 below
    movq $0, %rcx

loop_start:

    #Always store to the first memory location
    movq %rcx, (%r8)

    #Save the correct results
    movq %r8, %r10
    movq %rcx, %r11

    # When the loop count is even, do not swap the load location
    testq $1, %rcx
    je no_swap
    movq %r9, %r10
    movq $42, %r11
no_swap:

    #Perform and check the load
    movq (%r10), %rax
    cmp %rax, %r11
    jne fail

    #Equivlent to `if ((++rcx) != 400 ) goto loop_start;`
    incq %rcx
    cmp $400, %rcx
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
