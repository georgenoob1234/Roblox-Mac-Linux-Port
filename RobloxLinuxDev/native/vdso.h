// Darling's public __linux_syscall returns int and forwards six Linux arguments.
// Its inline _os_cpu_number uses this for every allocator CPU query. Reuse the
// kernel's vDSO instead of entering the kernel on each allocation.
static int (*trackb_vdso_getcpu)(unsigned *,unsigned *,void *);
static int (*trackb_vdso_clock_gettime)(int,void *);
static void trackb_init_vdso(void *library,void *(*symbol)(void *,const char *)) {
    if(!library)return;
    __atomic_store_n(&trackb_vdso_getcpu,(__typeof__(trackb_vdso_getcpu))symbol(library,"__vdso_getcpu"),__ATOMIC_RELEASE);
    __atomic_store_n(&trackb_vdso_clock_gettime,(__typeof__(trackb_vdso_clock_gettime))symbol(library,"__vdso_clock_gettime"),__ATOMIC_RELEASE);
}
int __linux_syscall(int nr,long a1,long a2,long a3,long a4,long a5,long a6) {
    if(nr==309) {
        __typeof__(trackb_vdso_getcpu) fast=__atomic_load_n(&trackb_vdso_getcpu,__ATOMIC_ACQUIRE);
        if(fast)return fast((unsigned *)a1,(unsigned *)a2,(void *)a3);
    }
    // Identical register convention and raw negative errno to Darling's
    // linux-syscall.S; also works before the vDSO is initialized.
    register long r10 __asm__("r10")=a4,r8 __asm__("r8")=a5,r9 __asm__("r9")=a6;
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"((long)nr),"D"(a1),"S"(a2),"d"(a3),
                     "r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory","cc");
    return (int)result;
}
int trackb_boottime(void *time) {
    __typeof__(trackb_vdso_clock_gettime) fast=__atomic_load_n(&trackb_vdso_clock_gettime,__ATOMIC_ACQUIRE);
    // Linux CLOCK_BOOTTIME=7 includes suspend, as Darwin CLOCK_MONOTONIC does.
    return fast?fast(7,time):__linux_syscall(228,7,(long)time,0,0,0,0);
}
