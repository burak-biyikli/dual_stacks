# Forwarding between loop iterations test
#
# Exercises the offset detection of the training logic in commit.
# Has a loop that forwards between the current and current/prior/2 iterations prior
#
# Return 0 on success, 1 on correctness failure.

.global _start

#Dummy memory location for the pairs of loads and stores to access
.section .bss
    .lcomm mem_array, 24

.text
_start:

    #Load the effective address of the three locations
    lea mem_array(%rip), %r8
    lea mem_array+8(%rip), %r9
    lea mem_array+16(%rip), %r10

    #Values expected for our load pairs (r8/r11, r9/r12, r10/r13)
    mov $7, %r11
    mov $3, %r12
    mov $1, %r13

    # Use rcx as loop counter, counting up to 100 iters
    mov $0, %rcx

loop_start:

    #Store the current pointer and value
    mov %r11, (%r8)

    #Determine if we are in a setup iteration
    cmp  $8, %rcx
    jl   skip_value_check
value_check:
    #Check offset zero (same loop)
    mov (%r8), %rax
    cmp %rax, %r11
    jne fail
    #Check offset one (prior loop)
    mov (%r9), %rax
    cmp %rax, %r12
    jne fail
    #Check offset two (two loop iters prior)
    mov (%r10), %rax
    cmp %rax, %r13
    jne fail
skip_value_check:


    # Rotate the pointer value pairs (Where value before -> value after)
    # Pointers: r9  -> r10, r8  -> r9,  r10 -> r8,
    # Data:     r12 -> r13, r11 -> r12, r11*2+1 -> r11

    #Rotate the pointers via swaps
    xchg %r8, %r9  # r9 holds (r8`), r8 holds(r9`)
    xchg %r10, %r8 # r9 holds (r8`), r8 holds(r10`), r10 holds (r9`)


    #Rotate the old value out, and new value in
    mov %r12, %r13
    mov %r11, %r12
    add %r11, %r11
    add $1, %r11

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
