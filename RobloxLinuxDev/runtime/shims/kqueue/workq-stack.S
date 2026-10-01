.text
.globl _rbx_workq_restack
.p2align 4
_rbx_workq_restack:
    movq %r9, %rsp
    andq $-16, %rsp
    subq $8, %rsp
    leaq Lunexpected_return(%rip), %rax
    movq %rax, (%rsp)
    xorq %rbp, %rbp
    jmpq *%r8
Lunexpected_return:
    ud2
