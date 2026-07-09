# Changing Cependency
# Covers the case of repeated and sequential squashes. Intentionally trains the
# predictor using a given pattern, then induces failues by swapping the dependency
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

    #Values expected for our load pairs
    movq $127, %r10
    movq $511, %r11

    # Use rcx as loop counter, 400 iters
    movq $400, %rcx

loop_start:

    addq $1, %r10
    addq $2, %r11

    # PC_store1 and PC_store2
    movq %r10, (%r8)
    movq %r11, (%r9)


    # When RCX & 0xF == 0xF (Every 16th iteration) we swap the pointers and
    # expected data. The swap will switch which stores the loads rely on,
    # causing many squashes/failed predictions. Depending on implementation
    # The swap in ST->LD dependency may reset the tracking for subsequent loops
    # or the LD PC may be marked unstable until evicted from the predictor.
    movq %rcx, %rax
    andq $0x0F, %rax
    cmpq $0x0F, %rax
    jne no_swap
do_swap:
    # Swap pointers and expected data
    xchg %r8,  %r9
    xchg %r10, %r11
no_swap:

    #Load block 1 depends on PC_store1 except during the swap case.
    movq (%r8), %r12
    movq (%r8), %r13
    movq (%r8), %r14
    movq (%r8), %r15
    cmp %r12, %r10
    jne fail
    cmp %r13, %r10
    jne fail
    cmp %r14, %r10
    jne fail
    cmp %r15, %r10
    jne fail

    #Load block 2 depends on PC_store2 except during the swap case.
    movq (%r9), %r12
    movq (%r9), %r13
    movq (%r9), %r14
    movq (%r9), %r15
    cmp %r12, %r11
    jne fail
    cmp %r13, %r11
    jne fail
    cmp %r14, %r11
    jne fail
    cmp %r15, %r11
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
