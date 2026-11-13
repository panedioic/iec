// compat/sigchain.c — chain-preserving crash forensics for preload guests.
//
// The loader route arms fault handlers before entering a guest image, but a
// guest runtime that installs its own SIGSEGV handler -- Bun's crash reporter
// and V8's wasm trap machinery are the concrete cases -- REPLACES it, and a
// guest crash then dies without a single line of forensics (measured with the
// Bun-compiled kimi binary: `kimi --version` crashes intermittently, output
// empty, BSH_FAULT_TRACE or not).
//
// Under INT_PRELOAD this module interposes sigaction(2): the handler the
// guest asks for is remembered per signal and our forensics dispatcher takes
// its place. On a fault the dispatcher prints pc/sp/lr/fp-chain (raw
// write(2), gated by BSH_FAULT_TRACE, once per process to stay crash-safe)
// and then invokes the remembered handler exactly as the kernel would have --
// so legitimate handler-based machinery (wasm trap resume, guard probes,
// crash reporters) keeps working, and every crash leaves its address behind.
//
// Only SIGSEGV/SIGBUS/SIGILL/SIGFPE are chained. signal(2) funnels into
// libc's internal sigaction and is not intercepted; every runtime that
// matters calls sigaction directly. A constructor installs a 64 KiB
// alternate signal stack so the dispatcher never runs on a stack that may
// itself be the broken thing.
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IEC_SIGCHAIN_MAX 4
static const int iec_chained_sigs[IEC_SIGCHAIN_MAX] = {
    SIGSEGV, SIGBUS, SIGILL, SIGFPE,
};
static struct sigaction iec_saved[IEC_SIGCHAIN_MAX];
static int iec_saved_set[IEC_SIGCHAIN_MAX];

static int iec_sig_index(int sig) {
    for (int i = 0; i < IEC_SIGCHAIN_MAX; i++)
        if (iec_chained_sigs[i] == sig) return i;
    return -1;
}

static void iec_sigchain_dispatcher(int sig, siginfo_t* info, void* uctx) {
    static int printed = 0;
    if (!printed) {
        printed = 1;   // once per process: a printer crash must not loop
        const char* trace = getenv("BSH_FAULT_TRACE");
        if (trace && trace[0] && trace[0] != '0' && uctx) {
            ucontext_t* uc = (ucontext_t*)uctx;
#if defined(__aarch64__)
            /* glibc aarch64 mcontext: regs[0..30] = x0..x30, regs[31] = sp,
             * regs[32] = pc. lr names the caller of whatever faulted; the
             * frame-pointer walk below names the callers of THAT. */
            char b[768];
            int n = snprintf(b, sizeof b,
                             "[sigchain] sig=%d pc=%llx sp=%llx lr=%llx addr=%p",
                             sig,
                             (unsigned long long)uc->uc_mcontext.regs[32],
                             (unsigned long long)uc->uc_mcontext.regs[31],
                             (unsigned long long)uc->uc_mcontext.regs[30],
                             info ? info->si_addr : NULL);
            ssize_t r = write(2, b, (size_t)n);
            (void)r;
            unsigned long fp = uc->uc_mcontext.regs[29];
            int w = n;
            for (int i = 0; i < 12 && fp; i++) {
                unsigned long* f = (unsigned long*)fp;
                unsigned long next = f[0];
                unsigned long ret = f[1];
                if (next <= fp || next - fp > 0x100000) break;
                n = snprintf(b + w, sizeof b - (size_t)w, " %lx", ret);
                if (n <= 0 || (size_t)n >= sizeof b - (size_t)w) break;
                w += n;
                fp = next;
            }
            n = snprintf(b + w, sizeof b - (size_t)w, "\n");
            r = write(2, b + w - n, (size_t)n);
            (void)r;

            /* Dump the WHOLE /proc/self/maps of the dying child: the crash's
             * meaning lives in the layout around it (which reservation owns
             * the fault address, where the anon interpreter sits). Raw
             * open/read/write only. */
            {
                static const char hdr[] = "[sigchain] maps begin\n";
                (void)write(2, hdr, sizeof hdr - 1);
                int mf = open("/proc/self/maps", O_RDONLY);
                if (mf >= 0) {
                    char mbuf[4096];
                    ssize_t mn;
                    size_t total = 0;
                    while ((mn = read(mf, mbuf, sizeof mbuf)) > 0 &&
                           total < (1 << 20)) {
                        total += (size_t)mn;
                        (void)write(2, mbuf, (size_t)mn);
                    }
                    close(mf);
                }
                static const char end[] = "[sigchain] maps end\n";
                (void)write(2, end, sizeof end - 1);
            }
#else
            (void)uc;
#endif
        }
    }
    // Forward, in the shape the guest asked for. Returning from here is the
    // sigreturn: a guest handler that resumes from modified ucontext keeps
    // working, because it saw the very ucontext the kernel built.
    const struct sigaction* sa = &iec_saved[iec_sig_index(sig)];
    if (sa->sa_flags & SA_SIGINFO) {
        ((void (*)(int, siginfo_t*, void*))sa->sa_sigaction)(sig, info, uctx);
    } else if ((void*)sa->sa_handler != SIG_IGN &&
               (void*)sa->sa_handler != SIG_DFL) {
        ((void (*)(int))sa->sa_handler)(sig);
    }
}

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    static int (*real)(int, const struct sigaction*, struct sigaction*);
    if (!real) real = (int (*)(int, const struct sigaction*, struct sigaction*))
                          dlsym(RTLD_NEXT, "sigaction");
    if (!real) {
        errno = ENOSYS;
        return -1;
    }

    const int idx = iec_sig_index(signum);
    if (idx < 0 || !act) return real(signum, act, oldact);

    // Queries and default/ignore installs pass straight through.
    if ((void*)act->sa_handler == SIG_DFL || (void*)act->sa_handler == SIG_IGN)
        return real(signum, act, oldact);

    iec_saved[idx] = *act;
    iec_saved_set[idx] = 1;

    struct sigaction mine = *act;
    mine.sa_sigaction = iec_sigchain_dispatcher;
    mine.sa_flags |= SA_SIGINFO | SA_NODEFER;
    return real(signum, &mine, oldact);
}

/* mmap/munmap tracing: the intermittent uv_set_process_title crash faults on
 * a memset destination that is NOT mapped at all. Was it ever? Log every
 * mmap/munmap/mprotect the guest makes (a Bun startup does dozens, not
 * thousands) and the answer falls out of the failing run's log: the block is
 * either absent from the mmap trail (allocation returned unmapped memory --
 * a malloc/brk problem) or removed by a later munmap (a teardown race). */
static int iec_mtrace_on(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("BSH_MTRACE");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

static void iec_mtrace_log(const char* what, unsigned long a, unsigned long b) {
    char b8[96];
    int n = snprintf(b8, sizeof b8, "[mtrace] %s %lx %lx\n", what, a, b);
    if (n > 0) {
        ssize_t r = write(2, b8, (size_t)n);
        (void)r;
    }
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static void* (*real)(void*, size_t, int, int, int, off_t);
    if (!real) real = (void* (*)(void*, size_t, int, int, int, off_t))
                          dlsym(RTLD_NEXT, "mmap");
    void* ret = real(addr, length, prot, flags, fd, offset);
    if (iec_mtrace_on() && ret != MAP_FAILED)
        iec_mtrace_log("mmap", (unsigned long)ret, length);
    return ret;
}

int munmap(void* addr, size_t length) {
    static int (*real)(void*, size_t);
    if (!real) real = (int (*)(void*, size_t))dlsym(RTLD_NEXT, "munmap");
    int ret = real(addr, length);
    if (iec_mtrace_on()) iec_mtrace_log("munmap", (unsigned long)addr, length);
    return ret;
}

/* An alternate stack of our own, set as early as LD_PRELOAD allows: the
 * dispatcher runs with SA_ONSTACK, so a guest crash on a smashed stack still
 * reaches the forensics. A guest that installs its own altstack later simply
 * overrides this one -- also fine. */
__attribute__((constructor)) static void iec_sigchain_init(void) {
    static char stack[64 * 1024] __attribute__((aligned(16)));
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = stack;
    ss.ss_size = sizeof stack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
}
