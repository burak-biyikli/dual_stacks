# No Forwarding Synthetic Test
# Designed to thwart the Memory Renaming Predictor by alternating the dependency between
# loads and their producing stores, preventing the predictor from gaining confidence.

.global _start

#Dummy memory location for the pair of loads and stores to access
.section .bss
    .lcomm mem_array, 16

.text
_start:

    #Load the effective address of the two memory locations above into r8 and r9
    lea mem_array(%rip), %r8
    lea mem_array+8(%rip), %r9

    #Values expected for our load pairs
    mov $127, %r10
    mov $511, %r11

    # Use rcx as loop counter, 100 iters
    mov $100, %rcx

loop_start:

    addq $1, %r10
    addq $2, %r11

    # PC_store1
    mov %r10, (%r8)
    # PC_store2
    mov %r11, (%r9)


    # If the loop count is even, do not perform a swap
    testq $1, %rcx
    je no_swap
    # Swap pointers and expected data
    # Without this swap, Load 0 depends on store 0, after the swap
    # Load 0 will load from address that was in r9 instead of r8 and depend on store 1
    xchg %r8,  %r9
    xchg %r10, %r11
no_swap:

    # PC_load1 (Matches with PC_store1 on even counts, PC_store2 on odd)
    # and checks that we load the right value
    mov (%r8), %rax
    cmp %rax, %r10
    jne fail

    # PC_load2 (matches with PC_store2 on even counts, PC_store1 on odd)
    # and checks that we load the right value
    mov (%r9), %rax
    cmp %rax, %r11
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
