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

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

pid_t SpawnLoader(const SpawnRequest& req) {
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

    /* Same reason as the old procmgr: a foreign glibc registers rseq during
     * startup and the sandbox turns that syscall into SIGSYS. The bundled
     * glibc is patched, but guests carrying their own libc still need it. */
    EnsureGlibcTunable(&env);

    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (std::string& s : env) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);
    environ = envp.data();

    const int argc = (int)req.argv.size();

    // A dynamic image is entered through its interpreter: map both images and
    // start at the interpreter's entry, which relocates the main image and
    // calls its entry. A static image is entered directly.
    const std::string interp = ReadInterp(req.path);
    if (!interp.empty()) {
        std::vector<unsigned char> main_img, ldso_img;
        if (Slurp(req.path, &main_img) && ReadInterpImage(interp, &ldso_img)) {
            const int rc = elf_run_dyn_inproc(main_img.data(), main_img.size(),
                                              ldso_img.data(), ldso_img.size(),
                                              argc, argv.data());
            if (rc < 0) return -1;
            fflush(nullptr);
            _exit(rc & 0xff);
        }
        /* Do NOT fall through to elf_run_file() here: a dynamic ET_DYN entered
         * without its interpreter has no relocations and jumps to a raw entry
         * (SIGSEGV). Report the real problem and let the caller exit 127. */
        fprintf(stderr, "[exec] loader: cannot read %s or its interpreter %s\n",
                req.path.c_str(), interp.c_str());
        return -1;
    }

    const int rc = elf_run_file(req.path.c_str(), argc, argv.data());
    if (rc < 0) return -1;
    fflush(nullptr);
    _exit(rc & 0xff);
}
