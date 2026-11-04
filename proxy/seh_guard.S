/*
 * seh_guard.S - x86_64 Windows SEH guard for calls into hook code (GNU as, AT&T syntax).
 *
 * int rf_seh_call3(void *fn, void *a, void *b, void *c, unsigned long *exc_code);
 *
 *   Calls ((int (*)(void *, void *, void *))fn)(a, b, c) and returns its result.
 *   If an SEH exception (access violation, RaiseException, stack overflow, a C++
 *   exception thrown past fn, ...) is not handled inside fn, the language handler
 *   rf_seh_handler (proxy.c) unwinds back to rf_seh_recover with the exception code
 *   in eax; rf_seh_recover stores it in *exc_code (when exc_code != NULL) and returns
 *   0x80000000. *exc_code is left untouched when fn returns normally, so callers set
 *   it to 0 first and test it afterwards.
 *
 * Frame: no frame register, 40 bytes allocated (32 home space + 8 for alignment),
 * so the establisher frame equals rsp in the body and the 5th argument (exc_code)
 * is at 80(%rsp): 40 allocated + 8 return address + 32 caller home space.
 * The nop after the call keeps the return address out of the epilogue; an address
 * inside an epilogue would make the OS skip this frame's handler.
 */
    .text
    .p2align 4
    .globl  rf_seh_call3
    .def    rf_seh_call3; .scl 2; .type 32; .endef
    .seh_proc rf_seh_call3
rf_seh_call3:
    .seh_handler rf_seh_handler, @except
    subq    $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movq    %rcx, %rax
    movq    %rdx, %rcx
    movq    %r8, %rdx
    movq    %r9, %r8
    callq   *%rax
    nop
    addq    $40, %rsp
    ret

    .globl  rf_seh_recover
rf_seh_recover:
    movq    80(%rsp), %rcx
    testq   %rcx, %rcx
    je      1f
    movl    %eax, (%rcx)
1:
    movl    $0x80000000, %eax
    addq    $40, %rsp
    ret
    .seh_endproc
