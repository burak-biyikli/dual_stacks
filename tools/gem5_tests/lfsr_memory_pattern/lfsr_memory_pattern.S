# LFSR Memory Pattern Test (Iterative Kernels)
# Tests multiple store-load forwarding distances to train and trigger the 
# dynamic Memory Renaming Predictor (MRP) in the gem5 O3 pipeline.
#
# Return codes:
# 0 = Success
# 1 = Failed Kernel 1 (Write 1, Read 1)
# 2 = Failed Kernel 2 (Write 2, Read 2)
# 3 = Failed Kernel 3 (Write 4, Read 4)
# 4 = Failed Kernel 4 (Write 50, Read 50)

.global _start

.section .bss
    .lcomm mem_array, 400   # 50 elements * 8 bytes

.text

.macro XORSHIFT64 state_reg, tmp_reg
    mov \state_reg, \tmp_reg
    shl $13, \tmp_reg
    xor \tmp_reg, \state_reg

    mov \state_reg, \tmp_reg
    shr $7, \tmp_reg
    xor \tmp_reg, \state_reg

    mov \state_reg, \tmp_reg
    shl $17, \tmp_reg
    xor \tmp_reg, \state_reg
.endm

_start:
    lea mem_array(%rip), %r12

    # ==========================================
    # Kernel 1: Write 1, Read 1 (Distance = 1)
    # Tightly coupled, guaranteed to overlap in ROB and train MRP.
    # ==========================================
    mov $1, %r13               # Seed
    xor %r14, %r14             # Counter

k1_loop:
    cmp $50, %r14
    je k1_pass

    XORSHIFT64 %r13, %rax
    mov %r13, (%r12, %r14, 8)  # Write
    
    # Read immediately (forwarding opportunity!)
    mov (%r12, %r14, 8), %rbx  
    cmp %r13, %rbx
    jne k1_fail

    inc %r14
    jmp k1_loop

k1_fail:
    mov $1, %rdi
    jmp exit
k1_pass:

    # ==========================================
    # Kernel 2: Write 2, Read 2 (Distance = 2)
    # ==========================================
    mov $1, %r13               
    xor %r14, %r14             

k2_loop:
    cmp $50, %r14
    je k2_pass

    XORSHIFT64 %r13, %rax
    mov %r13, 0(%r12, %r14, 8)
    
    mov %r13, %r15             # Backup to generate next
    XORSHIFT64 %r15, %rax
    mov %r15, 8(%r12, %r14, 8)

    # Read back 2
    mov 0(%r12, %r14, 8), %rbx  
    cmp %r13, %rbx
    jne k2_fail

    mov 8(%r12, %r14, 8), %rbx  
    cmp %r15, %rbx
    jne k2_fail

    # Advance LFSR state natively for next loop
    mov %r15, %r13
    add $2, %r14
    jmp k2_loop

k2_fail:
    mov $2, %rdi
    jmp exit
k2_pass:

    # ==========================================
    # Kernel 3: Write 4, Read 4 (Distance = 4)
    # ==========================================
    mov $1, %r13               
    xor %r14, %r14             

k3_loop:
    cmp $48, %r14              # Process in chunks of 4 (48 is divisible)
    je k3_pass

    # Unroll 4 writes (using stack / regs to keep code simple, just inline generate)
    mov %r13, %rcx
    
    XORSHIFT64 %rcx, %rax
    mov %rcx, 0(%r12, %r14, 8)
    mov %rcx, %r8
    
    XORSHIFT64 %rcx, %rax
    mov %rcx, 8(%r12, %r14, 8)
    mov %rcx, %r9

    XORSHIFT64 %rcx, %rax
    mov %rcx, 16(%r12, %r14, 8)
    mov %rcx, %r10

    XORSHIFT64 %rcx, %rax
    mov %rcx, 24(%r12, %r14, 8)
    mov %rcx, %r11

    # Unroll 4 reads
    mov 0(%r12, %r14, 8), %rbx  
    cmp %r8, %rbx
    jne k3_fail

    mov 8(%r12, %r14, 8), %rbx  
    cmp %r9, %rbx
    jne k3_fail

    mov 16(%r12, %r14, 8), %rbx  
    cmp %r10, %rbx
    jne k3_fail

    mov 24(%r12, %r14, 8), %rbx  
    cmp %r11, %rbx
    jne k3_fail

    mov %r11, %r13
    add $4, %r14
    jmp k3_loop

k3_fail:
    mov $3, %rdi
    jmp exit
k3_pass:

    # ==========================================
    # Kernel 4: Write 50, Read 50
    # Overflows typical ROB windows, testing LSU fallback
    # ==========================================
    mov $1, %r13               
    xor %r14, %r14             

k4_write_loop:
    cmp $50, %r14
    je k4_read_phase

    XORSHIFT64 %r13, %rax
    mov %r13, (%r12, %r14, 8)  
    inc %r14
    jmp k4_write_loop

k4_read_phase:
    mov $1, %r13               
    xor %r14, %r14             

k4_read_loop:
    cmp $50, %r14
    je success

    XORSHIFT64 %r13, %rax
    mov (%r12, %r14, 8), %rbx  
    cmp %r13, %rbx
    jne k4_fail

    inc %r14
    jmp k4_read_loop

k4_fail:
    mov $4, %rdi
    jmp exit

success:
    xor %rdi, %rdi

exit:
    mov $60, %rax
    syscall
