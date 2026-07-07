# Reverse Polish Notation (RPN) Calculator Test
#
# Loops a stack-based RPN evaluation to ensure the PCs are executed 
# multiple times. This allows the LSQ to train the Stack Renaming Predictor (SRP) 
# and Memory Renaming Predictor (MRP) on early iterations, so forwarding 
# cleanly engages on later iterations.

.global _start

.text

.macro RPN_PUSH val
    pushq \val
.endm

.macro RPN_ADD
    popq %rax
    popq %rbx
    addq %rbx, %rax
    pushq %rax
.endm

.macro RPN_SUB
    popq %rbx
    popq %rax
    subq %rbx, %rax
    pushq %rax
.endm

.macro RPN_MUL
    popq %rax
    popq %rbx
    imulq %rbx, %rax
    pushq %rax
.endm

_start:
    # Run the evaluation 50 times to train predictors
    mov $50, %r15

evaluation_loop:
    cmp $0, %r15
    je success

    # ===============================================
    # Eq 1: 10 20 30 + + 5 * 100 -  (Result: 200)
    # ===============================================
    RPN_PUSH $10
    RPN_PUSH $20
    RPN_PUSH $30
    
    RPN_ADD      
    RPN_ADD      
    
    RPN_PUSH $5
    RPN_MUL      
    
    RPN_PUSH $100
    RPN_SUB      
    
    # Check result
    popq %rax
    cmp $200, %rax
    jne failure_eq1

    # ===============================================
    # Eq 2: 2 3 4 5 6 + * + * (Result: 94)
    # Stresses deeper pops triggering SRP LIFO matching
    # ===============================================
    RPN_PUSH $2
    RPN_PUSH $3
    RPN_PUSH $4
    RPN_PUSH $5
    RPN_PUSH $6

    RPN_ADD
    RPN_MUL
    RPN_ADD
    RPN_MUL

    popq %rax
    cmp $94, %rax
    jne failure_eq2

    dec %r15
    jmp evaluation_loop

failure_eq1:
    mov $1, %rdi
    mov $60, %rax
    syscall

failure_eq2:
    mov $2, %rdi
    mov $60, %rax
    syscall

success:
    xor %rdi, %rdi
    mov $60, %rax
    syscall
