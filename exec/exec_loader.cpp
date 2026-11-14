// exec/exec_loader.cpp — the kLoader route.
//
// The caller is already a child of the server (see StartByKind), so mapping
// and entering the image here is safe: a guest's exit_group ends only this
// child. Dynamic images are entered through their PT_INTERP; static images
// are entered at their own entry.
//
// The interpreter is looked up by basename in the bundle's native-library
// directory, not at the literal PT_INTERP path. The sandbox has no /lib (its
// glibc lives under /data/storage/el1/bundle/libs/arm64), so a dynamic guest
// whose PT_INTERP says /lib/ld-linux-aarch64.so.1 would otherwise never start.
// The same directory is added to LD_LIBRARY_PATH so the interpreter can find
// libc.so.6, libpthread.so.0, libstdc++.so.6 and the rest.
#include "exec_loader.h"

#include "../common/constant.h"
#include "../loader/loader.h"
#include "logger/logger.h"

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <string>
#include <vector>

extern char** environ;

namespace {

bool Slurp(const std::string& path, std::vector<unsigned char>* out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }

    const size_t len = (size_t)st.st_size;
    out->resize(len);
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, out->data() + got, len - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != len) {
        out->clear();
        return false;
    }
    return true;
}

// The interpreter named by PT_INTERP, or empty when the image is static.
std::string ReadInterp(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::string();

    Elf64_Ehdr eh;
    if (read(fd, &eh, sizeof eh) != (ssize_t)sizeof eh ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0) {
        close(fd);
        return std::string();
    }

    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    if (lseek(fd, (off_t)eh.e_phoff, SEEK_SET) < 0 ||
        read(fd, ph.data(), ph.size() * sizeof(Elf64_Phdr)) !=
            (ssize_t)(ph.size() * sizeof(Elf64_Phdr))) {
        close(fd);
        return std::string();
    }

    std::string interp;
    for (const Elf64_Phdr& p : ph) {
        if (p.p_type != PT_INTERP || p.p_filesz == 0 || p.p_filesz > 4096) continue;
        std::vector<char> buf((size_t)p.p_filesz + 1, '\0');
        if (lseek(fd, (off_t)p.p_offset, SEEK_SET) < 0 ||
            read(fd, buf.data(), (size_t)p.p_filesz) != (ssize_t)p.p_filesz) {
            break;
        }
        interp.assign(buf.data(), strnlen(buf.data(), (size_t)p.p_filesz));
        break;
    }
    close(fd);
    return interp;
}

std::string BaseName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string DirName(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// ── SIGSYS downgrade, same semantics the old procmgr installed ──────────────
//
// The sandbox's seccomp does not return an errno for unknown syscalls: with
// SECCOMP_RET_TRAP it raises SIGSYS and kills the process (rc=159) unless a
// handler is installed. Returning normally from the handler makes the trapped
// call end with -ENOSYS, which is exactly what a real kernel without the
// syscall does -- and what guests expect (libuv falls back to epoll, glibc to
// the non-rseq/clone3 paths, ...). Guests run in this very process, so a
// handler installed before entering the image covers the whole guest.
//
// The handler must be async-signal-safe: no printf/malloc/getenv. The trace
// switch is read once, up front, and the number is written by hand.

static int g_sigsys_trace = 0;

static void SigSysWriteDec(int fd, long v) {
    char b[24];
    int i = (int)sizeof b;
    bool neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) b[--i] = '0';
    while (u) { b[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[--i] = '-';
    ssize_t r = write(fd, b + i, sizeof b - (size_t)i);
    (void)r;
}

static void SigSysToEnosys(int, siginfo_t* info, void*) {
    if (g_sigsys_trace) {
        static const char pre[] = "[exec] SIGSYS syscall=";
        if (write(STDERR_FILENO, pre, sizeof pre - 1)) {}
        // si_syscall is stable since Linux 4.14; 0 when unavailable.
        SigSysWriteDec(STDERR_FILENO, info ? (long)info->si_syscall : 0);
        static const char nl[] = "\n";
        if (write(STDERR_FILENO, nl, 1)) {}
    }
    /* Return: the kernel completes the trapped syscall with -ENOSYS. */
}

static void InstallSigSysDowngrade() {
    g_sigsys_trace = getenv("IEC_SIGSYS_TRACE") ? 1 : 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = SigSysToEnosys;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, nullptr);
}

// "K=V" vector helpers. The environment passed to elf_run_dyn_inproc is the
// vector built below, not the process's own env, so changes have to land there.
std::string EnvGet(const std::vector<std::string>& env, const char* key) {
    const std::string prefix = std::string(key) + "=";
    for (const std::string& e : env) {
        if (e.compare(0, prefix.size(), prefix) == 0) return e.substr(prefix.size());
    }
    return std::string();
}

void EnvSet(std::vector<std::string>* env, const char* key, const std::string& value) {
    const std::string prefix = std::string(key) + "=";
    for (std::string& e : *env) {
        if (e.compare(0, prefix.size(), prefix) == 0) {
            e = prefix + value;
            return;
        }
    }
    env->push_back(prefix + value);
}

void EnvPrepend(std::vector<std::string>* env, const char* key, const std::string& item) {
    if (item.empty()) return;
    const std::string prefix = std::string(key) + "=";
    for (std::string& e : *env) {
        if (e.compare(0, prefix.size(), prefix) == 0) {
            const std::string old = e.substr(prefix.size());
            e = prefix + (old.empty() ? item : item + ":" + old);
            return;
        }
    }
    env->push_back(prefix + item);
}

void EnsureGlibcTunable(std::vector<std::string>* env) {
    static const char kItem[] = "glibc.pthread.rseq=0";
    std::string old = EnvGet(*env, "GLIBC_TUNABLES");
    if (old.find(kItem) != std::string::npos) return;
    if (!old.empty()) old += ":";
    EnvSet(env, "GLIBC_TUNABLES", old + kItem);
}

// The preload shim (fs/exec/tty interception, INT_PRELOAD form) only exists for
// the libc it was compiled against: a musl-built shim preloaded into a glibc
// process crashes at load time and the other way round, which is why the old
// procmgr kept two builds and picked one by the guest's PT_INTERP. This host
// only ships the glibc build (the kLoader route is the glibc guest route; musl
// guests are command .so's entered through kSo, which carry the shim inside).
//
// Why the shim matters: the SIGSYS downgrade installed before entering the
// image dies with the process's first execve() (exec resets handlers), and the
// sandbox answers unknown syscalls with SIGSYS. The shim keeps the loop closed:
// its exec family delegates back to the exec server (which forks a fresh child
// and installs the downgrade again), and its syscall wrapper answers the
// io_uring_setup probe with ENOSYS before a signal is ever raised. This is the
// IEC counterpart of procmgr's ChildRunElf shim selection.
//
// The shim is libc-specific, so the pick is by the guest's interpreter, exactly
// the split ChildRunElf made: ld-linux -> libiec_preload.so (glibc ABI),
// ld-musl -> libiec_preload_musl.so (OHOS-musl ABI -- the musl guest actually
// runs on the bundle's ld-musl, see ReadInterpImage, and without its shim its
// execve lands on the kernel and dies: the musl testprogs lost exec self/
// sibling/dependent AND the fs/pty emulations to this, 6 of 31 checks). An
// empty interpreter (static guest) gets nothing -- a -static binary has no
// dynamic loader, so LD_PRELOAD cannot reach it.
void EnsureGuestShim(const std::string& interp, std::vector<std::string>* env) {
    const char* shim;
    if (interp.empty()) return;   // static guest: nothing can preload it
    if (interp.find("ld-musl") != std::string::npos) {
        shim = "libiec_preload_musl.so";
    } else {
        shim = "libiec_preload.so";
    }

    const std::string path = std::string(IEC_BUNDLE_LIBS) + shim;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;   // not shipped: stay compatible

    EnvPrepend(env, "LD_PRELOAD", path);
}

// Try the literal PT_INTERP path first (host builds and real /lib), then the
// same basename in the bundle's native-library directory and the fake sysroot.
bool ReadInterpImage(const std::string& interp, std::vector<unsigned char>* out) {
    if (interp.empty()) return false;

    const std::string base = BaseName(interp);
    std::vector<std::string> cands;
    /* Bundle first: on device this is the runtime the HAP was built with, and
     * it is also what the old procmgr used. The literal path is only a host
     * fallback; it must not shadow the bundle's loader. */
    if (!base.empty()) {
        cands.push_back(std::string(IEC_BUNDLE_LIBS) + base);
        cands.push_back(std::string(FAKE_SYSROOT) + "/lib/" + base);
        cands.push_back(std::string(FAKE_SYSROOT) + "/lib64/" + base);
    }
    cands.push_back(interp);
    for (const std::string& c : cands) {
        if (Slurp(c, out)) return true;
    }
    return false;
}

}  // namespace

// Give up the anonymous mappings below 512 MB before the image is placed.
//
// A non-PIE guest carries a fixed load address -- 0x400000 is the classic one
// -- and the loader reserves that range with MAP_FIXED_NOREPLACE. The forked
// child starts as a copy of the app process, whose low memory is occupied by
// one unnamed ~512 MB rw-p region (the ArkTS heap reservation,
// 0x140000-0x20040000) that covers exactly the addresses such a guest wants
// (measured with the Bun-compiled kimi binary: "reserve fixed image range
// [0x400000,+…) failed: File exists"). Unnamed regions count too -- only
// matching "[anon:" skipped that one, which is what the old procmgr got
// wrong before fixing it.
//
// The ceiling is 512 MB, NOT the 4 GB the old procmgr swept: everything this
// child still needs -- the anonymous BSS tails behind every library's data
// mapping (libiec_server's own globals among them: the sweep's first casualty
// was a silent SIGSEGV on the very first global write), malloc arenas, thread
// stacks -- lives above that line in this layout, so a 4 GB sweep disarms the
// process before the guest is even entered. The ArkTS reservation is the one
// region that actually blocks a non-PIE image; everything else can stay.
//
// This is safe here and only here: the child is disposable, and the sweep
// drops anonymous regions alone. File-backed mappings (host libc, libentry,
// arkweb's databases) stay -- this child keeps running on them, and dropping
// them would take the loader down with the guest.
//
// Order matters: run this before anything in the child allocates. Fresh
// mmaps land at the top of the address space in practice, but the whole
// point of the sweep is that low addresses are then free for the guest, and
// an allocation parked there first would be munmapped from under us.
static void UnmapLowAnonRegions() {
    const unsigned long cap = 0x20000000UL;   /* 512 MB, page-aligned */
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = {0};
        char path[256] = {0};
        const int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &lo, &hi, perms, path);
        if (n < 3 || hi <= lo) continue;
        /* Clip, don't skip: the ArkTS reservation ends at 0x20040000, just
         * past the cap. Dropping regions whose END exceeds the cap would
         * leave exactly the one region this sweep exists for mapped. */
        if (lo >= cap) continue;
        if (hi > cap) hi = cap;
        if (n < 4 || strncmp(path, "[anon:", 6) == 0)
            munmap((void*)lo, hi - lo);
    }
    fclose(f);
    /* Do NOT be tempted to also drop the high host-runtime regions:
     * [anon:native_heap:jemalloc] is NOT a foreign heap -- OHOS musl's native
     * allocator IS jemalloc, and those regions are the live malloc heap of
     * this very child (SpawnLoader's vectors, the image slurp, everything).
     * Sweeping them crashes the loader inside ld-musl's allocation path
     * (measured: pc in /system/lib/ld-musl-aarch64.so.1, rc=139, 8/8 runs).
     * The kimi startup flake that motivated the attempt (faulting memset
     * destination = a musl-heap region end scaled by 16, three samples, all
     * pre-Bun-sigaction) remains unsolved; compat/sigchain.c carries the
     * forensics that will name it. */
}

pid_t SpawnLoader(const SpawnRequest& req) {
    /* Forensics first: any crash from here on -- in the sweep, the slurp, the
     * image mapping, or the guest itself -- must be visible. A guest runtime
     * that installs its own SIGSEGV handler later replaces this one (the
     * preload shim's sigchain interposition re-chains in that case, see
     * compat/sigchain.c). */
    {
        const char* ft = getenv("BSH_FAULT_TRACE");
        bool want = ft && ft[0] && ft[0] != '0';
        for (const std::string& kv : req.env) {
            if (kv.rfind("BSH_FAULT_TRACE=", 0) == 0 && kv.size() > 16 && kv[16] != '0')
                want = true;
        }
        if (want) elf_arm_fault_handlers();
    }

    UnmapLowAnonRegions();

    if (!req.cwd.empty() && chdir(req.cwd.c_str()) != 0) {
        static const char msg[] = "[exec] chdir failed, started in default path\n";
        if (write(STDERR_FILENO, msg, sizeof msg - 1)) {}
    }

    /* Guests hit seccomp traps (rseq/io_uring/...) that kill the process with
     * SIGSYS; downgrading them to -ENOSYS is the whole guest survival story of
     * the old procmgr. Install before entering any image. */
    InstallSigSysDowngrade();

    std::vector<char*> argv;
    argv.reserve(req.argv.size() + 1);
    for (const std::string& s : req.argv) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    /* The loader will need the bundle's runtime and, for app-local layouts, the
     * executable's own directory. The guest directory goes first (an app's
     * same-named library must win over the bundle's), then the bundle libs.
     * The caller's existing LD_LIBRARY_PATH stays behind both. */
    std::vector<std::string> env = req.env;
    const std::string guest_dir = DirName(req.path);
    std::string libs = guest_dir;
    if (!libs.empty()) libs += ":";
    libs += IEC_BUNDLE_LIBS;
    EnvPrepend(&env, "LD_LIBRARY_PATH", libs);

    /* The interpreter decides more than the image entry: the preload shim is
     * libc-specific, so read it before the environment is frozen. */
    const std::string interp = ReadInterp(req.path);

    /* Same reason as the old procmgr: a foreign glibc registers rseq during
     * startup and the sandbox turns that syscall into SIGSYS. The bundled
     * glibc is patched, but guests carrying their own libc still need it. */
    EnsureGlibcTunable(&env);

    /* /proc/self/exe for the guest: this process never exec'd (the image was
     * mapped in-process), so the kernel's /proc/self/exe still points at the
     * app binary -- and anything that locates itself through it (hdc spawning
     * its server is the concrete victim) would re-exec the app. The fs shim
     * answers readlink/stat of /proc/self/exe from IEC_GUEST_EXE; set it to
     * the guest's own path, exactly what procmgr's ChildRunElf did with
     * BSH_GUEST_EXE. */
    {
        char gxbuf[4096];
        const char* gx = realpath(req.path.c_str(), gxbuf) ? gxbuf : req.path.c_str();
        EnvSet(&env, "IEC_GUEST_EXE", gx);
    }

    /* Guest-internal execve must loop back into the exec server (handler +
     * environment reinstall on the new process); without the shim the first
     * exec resets the SIGSYS downgrade and the child dies on the next seccomp
     * trap (hdc start spawning its server is the concrete victim). */
    EnsureGuestShim(interp, &env);

    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (std::string& s : env) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);
    environ = envp.data();

    const int argc = (int)req.argv.size();

    // A dynamic image is entered through its interpreter: map both images and
    // start at the interpreter's entry, which relocates the main image and
    // calls its entry. A static image is entered directly.
    if (!interp.empty()) {
        std::vector<unsigned char> main_img, ldso_img;
        if (!Slurp(req.path, &main_img)) {
            Logger::GetInstance()->LogE("[exec] loader: cannot read %s", req.path.c_str());
            return -1;
        }
        if (!ReadInterpImage(interp, &ldso_img)) {
            Logger::GetInstance()->LogE("[exec] loader: cannot read interpreter %s",
                                        interp.c_str());
            return -1;
        }
        const int rc = elf_run_dyn_inproc(main_img.data(), main_img.size(),
                                          ldso_img.data(), ldso_img.size(),
                                          argc, argv.data());
        if (rc < 0) {
            Logger::GetInstance()->LogE(
                "[exec] loader: in-process load of %s failed rc=%d errno=%d",
                req.path.c_str(), rc, errno);
            return -1;
        }
        fflush(nullptr);
        _exit(rc & 0xff);
    }

    const int rc = elf_run_file(req.path.c_str(), argc, argv.data());
    if (rc < 0) {
        Logger::GetInstance()->LogE("[exec] loader: static load of %s failed errno=%d",
                                    req.path.c_str(), errno);
        return -1;
    }
    fflush(nullptr);
    _exit(rc & 0xff);
}
