// The Switch's user exception entry (from DuckStation's Switch port). The
// kernel enters here with x0 = reason and x1 = the exception frame, having
// saved only x0-x8, lr, sp and pc there: the rest of the caller-saved state
// is kept on a dedicated stack around switch_exception_handler, and the
// kernel restores the thread from the frame on svcReturnFromException.

.text

.extern switch_exception_stack_top
.extern switch_exception_handler

#define EXCEPTION_STACK_SIZE 0x8000

.global __libnx_exception_entry
.type __libnx_exception_entry, %function
__libnx_exception_entry:
    adrp x2, switch_exception_stack_top
    add x2, x2, #:lo12:switch_exception_stack_top
    add sp, x2, EXCEPTION_STACK_SIZE

    sub sp, sp, #16*37

    // caller-saved registers the frame does not hold
    stp x9, x10, [sp]
    stp x11, x12, [sp, #16*1]
    stp x13, x14, [sp, #16*2]
    stp x15, x16, [sp, #16*3]
    stp x17, x18, [sp, #16*4]

    // all vector registers (only the low halves of v8-v15 are callee-saved)
    stp q0, q1, [sp, #16*5]
    stp q2, q3, [sp, #16*7]
    stp q4, q5, [sp, #16*9]
    stp q6, q7, [sp, #16*11]
    stp q8, q9, [sp, #16*13]
    stp q10, q11, [sp, #16*15]
    stp q12, q13, [sp, #16*17]
    stp q14, q15, [sp, #16*19]
    stp q16, q17, [sp, #16*21]
    stp q18, q19, [sp, #16*23]
    stp q20, q21, [sp, #16*25]
    stp q22, q23, [sp, #16*27]
    stp q24, q25, [sp, #16*29]
    stp q26, q27, [sp, #16*31]
    stp q28, q29, [sp, #16*33]
    stp q30, q31, [sp, #16*35]

    // x0 = reason, x1 = frame, x2 = the faulting code's frame pointer
    mov x2, x29
    bl switch_exception_handler

    ldp q0, q1, [sp, #16*5]
    ldp q2, q3, [sp, #16*7]
    ldp q4, q5, [sp, #16*9]
    ldp q6, q7, [sp, #16*11]
    ldp q8, q9, [sp, #16*13]
    ldp q10, q11, [sp, #16*15]
    ldp q12, q13, [sp, #16*17]
    ldp q14, q15, [sp, #16*19]
    ldp q16, q17, [sp, #16*21]
    ldp q18, q19, [sp, #16*23]
    ldp q20, q21, [sp, #16*25]
    ldp q22, q23, [sp, #16*27]
    ldp q24, q25, [sp, #16*29]
    ldp q26, q27, [sp, #16*31]
    ldp q28, q29, [sp, #16*33]
    ldp q30, q31, [sp, #16*35]

    ldp x9, x10, [sp]
    ldp x11, x12, [sp, #16*1]
    ldp x13, x14, [sp, #16*2]
    ldp x15, x16, [sp, #16*3]
    ldp x17, x18, [sp, #16*4]

    // the kernel restores everything else (sp included) from the frame
    mov x0, 0
    bl svcReturnFromException
