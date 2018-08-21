// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "llvm-version.h"
#include <map>
#include <string>
#include <cstdio>
#include <llvm/Support/Host.h>
#include "julia.h"
#include "julia_internal.h"
#include "processor.h"
#include "julia_assert.h"

#ifndef _OS_WINDOWS_
#include <sys/mman.h>
#if defined(_OS_DARWIN_) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

using namespace llvm;

// --- library symbol lookup ---

// map from user-specified lib names to handles
static std::map<std::string, void*> libMap;
static jl_mutex_t libmap_lock;
extern "C"
void *jl_get_library(const char *f_lib)
{
    void *hnd;
#ifdef _OS_WINDOWS_
    if (f_lib == JL_EXE_LIBNAME)
        return jl_exe_handle;
    if (f_lib == JL_DL_LIBNAME)
        return jl_dl_handle;
#endif
    if (f_lib == NULL)
        return jl_RTLD_DEFAULT_handle;
    JL_LOCK_NOGC(&libmap_lock);
    // This is the only operation we do on the map, which doesn't invalidate
    // any references or iterators.
    void **map_slot = &libMap[f_lib];
    JL_UNLOCK_NOGC(&libmap_lock);
    hnd = jl_atomic_load_acquire(map_slot);
    if (hnd != NULL)
        return hnd;
    // We might run this concurrently on two threads but it doesn't matter.
    hnd = jl_load_dynamic_library(f_lib, JL_RTLD_DEFAULT);
    if (hnd != NULL)
        jl_atomic_store_release(map_slot, hnd);
    return hnd;
}

extern "C" JL_DLLEXPORT
void *jl_load_and_lookup(const char *f_lib, const char *f_name, void **hnd)
{
    void *handle = jl_atomic_load_acquire(hnd);
    if (!handle)
        jl_atomic_store_release(hnd, (handle = jl_get_library(f_lib)));
    return jl_dlsym(handle, f_name);
}

// miscellany
std::string jl_get_cpu_name_llvm(void)
{
    return llvm::sys::getHostCPUName().str();
}

std::string jl_get_cpu_features_llvm(void)
{
    StringMap<bool> HostFeatures;
    llvm::sys::getHostCPUFeatures(HostFeatures);
    std::string attr;
    for (auto &ele: HostFeatures) {
        if (ele.getValue()) {
            if (!attr.empty()) {
                attr.append(",+");
            }
            else {
                attr.append("+");
            }
            attr.append(ele.getKey().str());
        }
    }
    // Explicitly disabled features need to be added at the end so that
    // they are not re-enabled by other features that implies them by default.
    for (auto &ele: HostFeatures) {
        if (!ele.getValue()) {
            if (!attr.empty()) {
                attr.append(",-");
            }
            else {
                attr.append("-");
            }
            attr.append(ele.getKey().str());
        }
    }
    return attr;
}

extern "C" JL_DLLEXPORT
jl_value_t *jl_get_JIT(void)
{
    const std::string& HostJITName = "ORCJIT";
    return jl_pchar_to_string(HostJITName.data(), HostJITName.size());
}


static void *trampoline_freelist;

static void *trampoline_alloc()
{
    const int sz = 64; // oversized for most platforms. todo: use precise value?
    if (!trampoline_freelist) {
#ifdef _OS_WINDOWS_
        void *mem = VirtualAlloc(NULL, jl_page_size,
                MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
        void *mem = mmap(0, jl_page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        void *next = NULL;
        for (size_t i = 0; i + sz <= jl_page_size; i += sz) {
            void **curr = (void**)((char*)mem + i);
            *curr = next;
            next = (void*)curr;
        }
        trampoline_freelist = next;
    }
    void *tramp = trampoline_freelist;
    trampoline_freelist = *(void**)tramp;
    return tramp;
}

static void trampoline_free(void *tramp)
{
    *(void**)tramp = trampoline_freelist;
    trampoline_freelist = tramp;
}

static void trampoline_deleter(void **f)
{
    void *tramp = f[0];
    void *fobj = f[1];
    void *cache = f[2];
    void *nval = f[3];
    f[0] = NULL;
    f[2] = NULL;
    f[3] = NULL;
    if (tramp)
        trampoline_free(tramp);
    if (fobj && cache)
        ptrhash_remove((htable_t*)cache, fobj);
    if (nval)
        free(nval);
}

// TODO: need a thread lock around the cache access parts of this function
extern "C" JL_DLLEXPORT
jl_value_t *jl_get_cfunction_trampoline(
    // dynamic inputs:
    jl_value_t *fobj,
    jl_datatype_t *result_type,
    // call-site constants:
    htable_t *cache, // weakref htable indexed by (fobj, vals)
    jl_svec_t *fill,
    void *(*init_trampoline)(void *tramp, void **nval),
    jl_unionall_t *env,
    jl_value_t **vals)
{
    // lookup (fobj, vals) in cache
    if (!cache->table)
        htable_new(cache, 1);
    if (fill != jl_emptysvec) {
        htable_t **cache2 = (htable_t**)ptrhash_bp(cache, (void*)vals);
        cache = *cache2;
        if (cache == HT_NOTFOUND) {
            cache = htable_new((htable_t*)malloc(sizeof(htable_t)), 1);
            *cache2 = cache;
        }
    }
    void *tramp = ptrhash_get(cache, (void*)fobj);
    if (tramp != HT_NOTFOUND) {
        assert((jl_datatype_t*)jl_typeof(tramp) == result_type);
        return (jl_value_t*)tramp;
    }

    // not found, allocate a new one
    size_t n = jl_svec_len(fill);
    void **nval = (void**)malloc(sizeof(void**) * (n + 1));
    nval[0] = (void*)fobj;
    jl_value_t *result;
    JL_TRY {
        for (size_t i = 0; i < n; i++) {
            jl_value_t *sparam_val = jl_instantiate_type_in_env(jl_svecref(fill, i), env, vals);
            if (sparam_val != (jl_value_t*)jl_any_type)
                if (!jl_is_concrete_type(sparam_val) || !jl_is_immutable(sparam_val))
                    sparam_val = NULL;
            nval[i + 1] = (void*)sparam_val;
        }
        int permanent =
            (result_type == jl_voidpointer_type) ||
            jl_is_concrete_type(fobj) ||
            (((jl_datatype_t*)jl_typeof(fobj))->instance == fobj);
        if (jl_is_unionall(fobj)) {
            jl_value_t *uw = jl_unwrap_unionall(fobj);
            if (jl_is_datatype(uw) && ((jl_datatype_t*)uw)->name->wrapper == fobj)
                permanent = true;
        }
        if (permanent) {
            result = jl_gc_permobj(sizeof(jl_taggedvalue_t) + jl_datatype_size(result_type), result_type);
            memset(result, 0, jl_datatype_size(result_type));
        }
        else {
            result = jl_new_struct_uninit(result_type);
        }
        if (result_type != jl_voidpointer_type) {
            assert(jl_datatype_size(result_type) == sizeof(void*) * 4);
            ((void**)result)[1] = (void*)fobj;
        }
        if (!permanent) {
            void *ptr_finalizer[2] = {
                    (void*)jl_voidpointer_type,
                    (void*)&trampoline_deleter
                };
            jl_gc_add_finalizer(result, (jl_value_t*)&ptr_finalizer[1]);
            ((void**)result)[2] = (void*)cache;
            ((void**)result)[3] = (void*)nval;
        }
    }
    JL_CATCH {
        free(nval);
        jl_rethrow();
    }
    tramp = trampoline_alloc();
    ((void**)result)[0] = tramp;
    tramp = init_trampoline(tramp, nval);
    ptrhash_put(cache, (void*)fobj, result);
    return result;
}
