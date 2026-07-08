# No Forwarding (Size Mismatch) Synthetic Test
# Designed to thwart the Memory Renaming Predictor by storing small data types (32-bit)
# and loading large data types (64-bit). The mismatched sizes should force the
# predictor to abort forwarding, as the load depends on multiple distinct stores.

.global _start

.section .bss
    # We only need 8 bytes for this test
    .lcomm mem_array, 8

.text
_start:
    # Load the effective address of our memory location
    lea mem_array(%rip), %r8

    # Use rcx as loop counter, 100 iters
    mov $100, %rcx

    # Initial values for our 32-bit stores
    # (x86 refers to 32-bit as doublewords, 64-bit as quadwords)
    mov $0xAAAAAAAA, %r9d  # Lower 32 bits
    mov $0xBBBBBBBB, %r10d # Upper 32 bits

loop_start:
    # PC_store1: Write 32 bits to bytes 0-3
    movl %r9d, (%r8)

    # PC_store2: Write 32 bits to bytes 4-7
    movl %r10d, 4(%r8)

    # PC_load1: Read all 64-bits (bytes 0-7).
    # This load relies on BOTH preceding stores. A rename predictor that
    # maps 1 load -> 1 store will fail to forward this safely.
    movq (%r8), %rax

    # --- Reconstruct the expected 64-bit value to verify correctness ---
    # x86 is little-endian, so the expected value is (r10 << 32) | r9
    mov %r10, %r11
    shl $32, %r11          # Shift upper 32 bits into position

    # Ensure %r9 is treated as a zero-extended 64-bit value
    mov %r9d, %r9d         # (Writing to a 32-bit reg clears the upper 32-bits in x64)
    or %r9, %r11           # Combine lower and upper halves

    # Check that our quadword load fetched exactly what the two separate stores wrote
    cmp %rax, %r11
    jne fail

    # Mutate the values slightly for the next iteration
    # so the predictor doesn't just memorize a static value
    add $0x00000001, %r9d
    sub $0x00000001, %r10d

    # Equivalent to `if ((--rcx) != 0) goto loop_start;`
    dec %rcx
    jne loop_start

    # End of loop with no errors, return 0
    xor %rdi, %rdi
    jmp exit

fail:
    # Correctness error in load, return 1 for failure
    mov $1, %rdi
    jmp exit

exit:
    # syscall: exit
    mov $60, %rax
    syscall
