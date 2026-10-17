// elf_dl.cpp — architecture-specific parts of the in-memory dlopen fallback.
//
// Optional: this file is not part of the kLoader route and is not built by the
// current CMake/Makefile targets. The generic helpers it references are shared
// with the original implementation and are not repeated in this skeleton.
#include "loader.h"
#include <dlfcn.h>
#include <elf.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if ELFLOADER_ARCH_ARM64
extern "C" void bsh_tlsdesc_entry(void);
#endif

namespace {
pthread_mutex_t g_dl_lock = PTHREAD_MUTEX_INITIALIZER;

struct DlObj {
    uintptr_t base;
    size_t    span;
    char      soname[64];
    const Elf64_Sym  *symtab; size_t nsym;
    const char       *strtab; size_t strsz;
    const Elf64_Rela *rela;   size_t nrela;
    const Elf64_Rela *jmprel; size_t njmprel;
    bool used;
    uintptr_t tls_tmpl;
    size_t    tls_filesz, tls_memsz, tls_align;
    int       tls_key_ok;
    pthread_key_t tls_key;
};
DlObj g_objs[32];

static void *(*g_prefer_sym)(const char *name) = NULL;

/* elog / dlog / find_dynamic / collect_symtab / collect_relocs / sym_name /
 * resolve_sym / preload_needed / run_init / collect_tls / tls_block_for:
 * these are architecture-independent apart from the TLS pieces; copy them
 * from the original. */

/* ---------- TLS block: ARM uses pthread_key; x86 is the same, copy it ---------- */
/* collect_tls / tls_block_for are unchanged from the original */

/* ---------- relocations: architecture branches ---------- */
#if ELFLOADER_ARCH_ARM64

/* AArch64 TLSDESC record */
struct TlsDescRec { DlObj *obj; size_t off; };

static int apply_one(DlObj *o, const Elf64_Rela *r, const char *kind,
                     bool (*weak_ok)(DlObj*, size_t)) {
    const uint32_t type = (uint32_t)(r->r_info & 0xffffffff);
    const size_t   sidx = (size_t)(r->r_info >> 32);
    uintptr_t where = o->base + r->r_offset;
    uintptr_t val   = 0;
    switch (type) {
    case R_AARCH64_RELATIVE:
        *(uint64_t*)where = o->base + (uintptr_t)r->r_addend;
        return 0;
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
    case R_AARCH64_ABS64:
        val = resolve_sym(o, sidx);
        if (!val && sidx && !weak_ok(o,sidx)) return -1;
        *(uint64_t*)where = val + (uintptr_t)r->r_addend;
        return 0;
    case R_AARCH64_ABS32:
        val = resolve_sym(o, sidx);
        if (!val && sidx && !weak_ok(o,sidx)) return -1;
        *(uint32_t*)where = (uint32_t)(val + (uintptr_t)r->r_addend);
        return 0;
    case R_AARCH64_TLSDESC: {
        size_t off = (size_t)r->r_addend;
        if (sidx && sidx < o->nsym) off += (size_t)o->symtab[sidx].st_value;
        TlsDescRec *rec = (TlsDescRec*)malloc(sizeof(*rec));
        if (!rec) return -1;
        rec->obj = o; rec->off = off;
        uintptr_t *slot = (uintptr_t*)where;
        slot[0] = (uintptr_t)&bsh_tlsdesc_entry;
        slot[1] = (uintptr_t)rec;
        return 0;
    }
    case R_AARCH64_TLS_TPREL64:
    case R_AARCH64_TLS_DTPMOD64:
    case R_AARCH64_TLS_DTPREL64:
        elog("[dl] %s: unsupported TLS relocation type=%u", o->soname, type);
        return -1;
    default:
        elog("[dl] %s: unsupported %s reloc type %u", o->soname, kind, type);
        return -1;
    }
}

#elif ELFLOADER_ARCH_X86_64

/* x86_64 relocation constants used here (not every <elf.h> defines them):
 *   R_X86_64_NONE       0
 *   R_X86_64_64         1   S + A
 *   R_X86_64_PC32       2   S + A - P   (32-bit result)
 *   R_X86_64_GLOB_DAT   6   S
 *   R_X86_64_JUMP_SLOT  7   S
 *   R_X86_64_RELATIVE   8   B + A
 *   R_X86_64_32         10  S + A (32-bit zero-extended)
 *   R_X86_64_32S        11  S + A (32-bit sign-extended)
 *   R_X86_64_IRELATIVE  37  unsupported
 *   R_X86_64_DTPMOD64   16  unsupported
 *   R_X86_64_DTPOFF64   17  unsupported
 *   R_X86_64_TPOFF64    18  unsupported (static TLS)
 *   R_X86_64_TLSDESC    36  unsupported
 */
#ifndef R_X86_64_NONE
#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_32         10
#define R_X86_64_32S        11
#define R_X86_64_DTPMOD64   16
#define R_X86_64_DTPOFF64   17
#define R_X86_64_TPOFF64    18
#define R_X86_64_TLSDESC    36
#define R_X86_64_IRELATIVE  37
#endif

static int apply_one(DlObj *o, const Elf64_Rela *r, const char *kind,
                     bool (*weak_ok)(DlObj*, size_t)) {
    const uint32_t type = (uint32_t)(r->r_info & 0xffffffff);
    const size_t   sidx = (size_t)(r->r_info >> 32);
    uintptr_t where = o->base + r->r_offset;
    uintptr_t val   = 0;
    switch (type) {
    case R_X86_64_NONE:
        return 0;
    case R_X86_64_RELATIVE:
        *(uint64_t*)where = o->base + (uintptr_t)r->r_addend;
        return 0;
    case R_X86_64_64:
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
        val = resolve_sym(o, sidx);
        if (!val && sidx && !weak_ok(o,sidx)) return -1;
        *(uint64_t*)where = val + (uintptr_t)r->r_addend;
        return 0;
    case R_X86_64_32:
    case R_X86_64_32S:
        val = resolve_sym(o, sidx);
        if (!val && sidx && !weak_ok(o,sidx)) return -1;
        *(uint32_t*)where = (uint32_t)(val + (uintptr_t)r->r_addend);
        return 0;
    case R_X86_64_PC32: {
        val = resolve_sym(o, sidx);
        if (!val && sidx && !weak_ok(o,sidx)) return -1;
        int64_t v = (int64_t)(val + (uintptr_t)r->r_addend) - (int64_t)where;
        *(int32_t*)where = (int32_t)v;
        return 0;
    }
    case R_X86_64_TPOFF64:
    case R_X86_64_DTPMOD64:
    case R_X86_64_DTPOFF64:
    case R_X86_64_TLSDESC:
        elog("[dl] %s: unsupported TLS relocation type=%u (use -ftls-model=global-dynamic with __tls_get_addr, or link statically)",
             o->soname, type);
        return -1;
    case R_X86_64_IRELATIVE:
        elog("[dl] %s: IRELATIVE (STT_GNU_IFUNC) is not supported", o->soname);
        return -1;
    default:
        elog("[dl] %s: unsupported %s reloc type %u", o->soname, kind, type);
        return -1;
    }
}
#endif

int apply_relocs(DlObj *o) {
    auto weak_ok = +[](DlObj *o, size_t sidx) -> bool {
        return sidx < o->nsym &&
               ELF64_ST_BIND(o->symtab[sidx].st_info) == STB_WEAK;
    };
    for (size_t i=0;i<o->nrela;i++)
        if (apply_one(o, &o->rela[i],   "RELA",   weak_ok)!=0) return -1;
    for (size_t i=0;i<o->njmprel;i++)
        if (apply_one(o, &o->jmprel[i], "JMPREL", weak_ok)!=0) return -1;
    return 0;
}

} /* namespace */

#if ELFLOADER_ARCH_ARM64
/* bsh_tlsdesc_resolver_c is the same as the original; copy it. */
extern "C" uintptr_t bsh_tlsdesc_resolver_c(void *desc) {
    /* ... as in the original ... */
}
#endif

/* elf_dl_set_prefer_sym / dlopen_mem_impl / dlsym_impl /
 * elf_dlopen_mem / elf_dlsym / elf_dl_owns_handle: all identical to the original. */