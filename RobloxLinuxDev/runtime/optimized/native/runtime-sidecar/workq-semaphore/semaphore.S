// Nonnegative counting semaphore. Match the original helpers' RBP unwind ABI.
// Futex compares the count atomically, so a wake before WAIT cannot be lost.
.section .text.sem_down,"ax",@progbits
.global sem_down
.type sem_down,@function
sem_down:
    push %rbp
    mov %rsp,%rbp
    sub $32,%rsp
    mov %rdi,-8(%rbp)
    movslq %esi,%rax
    mov %rax,-24(%rbp)
    movq $0,-16(%rbp)
    lea -24(%rbp),%rax
    cmp $-1,%esi
    jne 1f
    xor %eax,%eax
1:  mov %rax,-32(%rbp)
2:  mov -8(%rbp),%rdi
    mov (%rdi),%eax
    test %eax,%eax
    jle 3f
    lea -1(%rax),%ecx
    lock cmpxchg %ecx,(%rdi)
    jne 2b
    mov $1,%eax
    leave
    ret
3:  xor %esi,%esi
    xor %edx,%edx
    mov -32(%rbp),%rcx
    xor %r8d,%r8d
    xor %r9d,%r9d
    call __linux_futex_reterr
    cmp $-60,%eax // Darwin ETIMEDOUT; EAGAIN and EINTR retry.
    jne 2b
    xor %eax,%eax
    leave
    ret
.size sem_down,.-sem_down

.section .text.sem_up,"ax",@progbits
.global sem_up
.type sem_up,@function
sem_up:
    push %rbp
    mov %rsp,%rbp
    lock incl (%rdi)
    mov $1,%esi
    mov $1,%edx
    xor %ecx,%ecx
    xor %r8d,%r8d
    xor %r9d,%r9d
    call __linux_futex_reterr
    pop %rbp
    ret
.size sem_up,.-sem_up
.section .note.GNU-stack,"",@progbits
