#include <c++/exception>
#include <c++/new>
#include <c++/typeinfo>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "cxx_unwind.h"

namespace __cxxabiv1 {

struct __upcast_result {
    const void *dst_ptr;
    int part2dst;
    int src_details;
    const __class_type_info *base_type;
};

class __class_type_info : public std::type_info {
public:
    enum __sub_kind {
        __unknown = 0,
        __not_contained,
        __contained_ambig,
        __contained_virtual_mask = 0x1,
        __contained_public_mask = 0x2,
        __contained_mask = 0x4,
        __contained_private = __contained_mask,
        __contained_public = __contained_mask | __contained_public_mask
    };

    explicit __class_type_info(const char *n) : std::type_info(n) {}
    virtual ~__class_type_info();

    virtual bool __do_catch(const std::type_info *thr_type, void **thr_obj, unsigned outer) const override;
    virtual bool __do_upcast(const __class_type_info *target, void **obj_ptr) const override;
    virtual bool __do_find_public_src(ptrdiff_t src2dst, const void *obj_ptr,
                                      const __class_type_info *src_type, const void *src_ptr) const;
    virtual bool __do_upcast2(const __class_type_info *target, const void *obj_ptr,
                              __upcast_result &result) const;
    virtual bool __do_dyncast(ptrdiff_t src2dst, __sub_kind access_path,
                              const __class_type_info *dst_type, const void *obj_ptr,
                              const __class_type_info *src_type, const void *src_ptr,
                              const void *&dst_ptr) const;
};

class __si_class_type_info : public __class_type_info {
public:
    const __class_type_info *__base_type;
    explicit __si_class_type_info(const char *n, const __class_type_info *base)
        : __class_type_info(n), __base_type(base) {}
    virtual ~__si_class_type_info();
    virtual bool __do_upcast2(const __class_type_info *target, const void *obj_ptr,
                              __upcast_result &result) const override;
    virtual bool __do_dyncast(ptrdiff_t src2dst, __sub_kind access_path,
                              const __class_type_info *dst_type, const void *obj_ptr,
                              const __class_type_info *src_type, const void *src_ptr,
                              const void *&dst_ptr) const override;
};

struct __base_class_type_info {
    const __class_type_info *__base_type;
    long __offset_flags;
    enum __offset_flags_masks {
        __virtual_mask = 0x1,
        __public_mask = 0x2,
        __hwm_bit = 2,
        __offset_shift = 8
    };
    bool __is_virtual_p() const { return (__offset_flags & __virtual_mask) != 0; }
    bool __is_public_p() const { return (__offset_flags & __public_mask) != 0; }
    ptrdiff_t __offset() const { return static_cast<ptrdiff_t>(__offset_flags) >> __offset_shift; }
};

class __vmi_class_type_info : public __class_type_info {
public:
    unsigned int __flags;
    unsigned int __base_count;
    __base_class_type_info __base_info[1];

    explicit __vmi_class_type_info(const char *n, int flags)
        : __class_type_info(n), __flags((unsigned)flags), __base_count(0) {}
    virtual ~__vmi_class_type_info();
    virtual bool __do_upcast2(const __class_type_info *target, const void *obj_ptr,
                              __upcast_result &result) const override;
    virtual bool __do_dyncast(ptrdiff_t src2dst, __sub_kind access_path,
                              const __class_type_info *dst_type, const void *obj_ptr,
                              const __class_type_info *src_type, const void *src_ptr,
                              const void *&dst_ptr) const override;
};

class __fundamental_type_info : public std::type_info {
public:
    explicit __fundamental_type_info(const char *n) : std::type_info(n) {}
    virtual ~__fundamental_type_info();
};

class __array_type_info : public std::type_info {
public:
    explicit __array_type_info(const char *n) : std::type_info(n) {}
    virtual ~__array_type_info();
};

class __function_type_info : public std::type_info {
public:
    explicit __function_type_info(const char *n) : std::type_info(n) {}
    virtual ~__function_type_info();
    virtual bool __is_function_p() const override { return true; }
};

class __enum_type_info : public std::type_info {
public:
    explicit __enum_type_info(const char *n) : std::type_info(n) {}
    virtual ~__enum_type_info();
};

class __pbase_type_info : public std::type_info {
public:
    unsigned int __flags;
    const std::type_info *__pointee;
    enum __masks {
        __const_mask = 0x1,
        __volatile_mask = 0x2,
        __restrict_mask = 0x4,
        __incomplete_mask = 0x8,
        __transaction_safe_mask = 0x20,
        __noexcept_mask = 0x40
    };
    explicit __pbase_type_info(const char *n, int quals, const std::type_info *type)
        : std::type_info(n), __flags((unsigned)quals), __pointee(type) {}
    virtual ~__pbase_type_info();
    virtual bool __do_catch(const std::type_info *thr_type, void **thr_obj, unsigned outer) const override;
};

class __pointer_type_info : public __pbase_type_info {
public:
    explicit __pointer_type_info(const char *n, int quals, const std::type_info *type)
        : __pbase_type_info(n, quals, type) {}
    virtual ~__pointer_type_info();
    virtual bool __is_pointer_p() const override { return true; }
};

class __pointer_to_member_type_info : public __pbase_type_info {
public:
    const __class_type_info *__context;
    explicit __pointer_to_member_type_info(const char *n, int quals, const std::type_info *type,
                                           const __class_type_info *klass)
        : __pbase_type_info(n, quals, type), __context(klass) {}
    virtual ~__pointer_to_member_type_info();
};

static const void *adjust_base_ptr(const void *obj, const __base_class_type_info &base) {
    if (!obj) return nullptr;
    if (base.__is_virtual_p()) {
        const char *const *vptr = *static_cast<const char *const *const *>(obj);
        ptrdiff_t voff = base.__offset();
        ptrdiff_t disp = *reinterpret_cast<const ptrdiff_t *>(reinterpret_cast<const char *>(vptr) + voff);
        return static_cast<const char *>(obj) + disp;
    }
    return static_cast<const char *>(obj) + base.__offset();
}

__class_type_info::~__class_type_info() {}
__si_class_type_info::~__si_class_type_info() {}
__vmi_class_type_info::~__vmi_class_type_info() {}
__fundamental_type_info::~__fundamental_type_info() {}
__array_type_info::~__array_type_info() {}
__function_type_info::~__function_type_info() {}
__enum_type_info::~__enum_type_info() {}
__pbase_type_info::~__pbase_type_info() {}
__pointer_type_info::~__pointer_type_info() {}
__pointer_to_member_type_info::~__pointer_to_member_type_info() {}

bool __class_type_info::__do_catch(const std::type_info *thr_type, void **thr_obj, unsigned outer) const {
    if (*this == *thr_type) return true;
    if (outer >= 4u) return false;
    return thr_type->__do_upcast(this, thr_obj);
}

bool __class_type_info::__do_upcast(const __class_type_info *target, void **obj_ptr) const {
    if (!obj_ptr) return false;
    __upcast_result res = { nullptr, __not_contained, 0, nullptr };
    if (!__do_upcast2(target, *obj_ptr, res)) return false;
    if ((res.part2dst & __contained_public) == __contained_public) {
        *obj_ptr = const_cast<void *>(res.dst_ptr);
        return true;
    }
    return false;
}

bool __class_type_info::__do_find_public_src(ptrdiff_t, const void *obj_ptr,
                                             const __class_type_info *src_type,
                                             const void *src_ptr) const {
    return (*this == *src_type) && (obj_ptr == src_ptr);
}

bool __class_type_info::__do_upcast2(const __class_type_info *target, const void *obj_ptr,
                                     __upcast_result &result) const {
    if (*this == *target) {
        result.dst_ptr = obj_ptr;
        result.part2dst = __contained_public;
        result.base_type = this;
        return true;
    }
    return false;
}

bool __class_type_info::__do_dyncast(ptrdiff_t, __sub_kind access_path,
                                     const __class_type_info *dst_type, const void *obj_ptr,
                                     const __class_type_info *, const void *,
                                     const void *&dst_ptr) const {
    if (*this == *dst_type && (access_path & __contained_public_mask)) {
        dst_ptr = obj_ptr;
        return true;
    }
    return false;
}

bool __si_class_type_info::__do_upcast2(const __class_type_info *target, const void *obj_ptr,
                                        __upcast_result &result) const {
    if (__class_type_info::__do_upcast2(target, obj_ptr, result)) return true;
    return __base_type->__do_upcast2(target, obj_ptr, result);
}

bool __si_class_type_info::__do_dyncast(ptrdiff_t src2dst, __sub_kind access_path,
                                        const __class_type_info *dst_type, const void *obj_ptr,
                                        const __class_type_info *src_type, const void *src_ptr,
                                        const void *&dst_ptr) const {
    if (*this == *dst_type && (access_path & __contained_public_mask)) {
        dst_ptr = obj_ptr;
        return true;
    }
    if (*this == *src_type && obj_ptr == src_ptr) {
        return false;
    }
    return __base_type->__do_dyncast(src2dst, access_path, dst_type, obj_ptr, src_type, src_ptr, dst_ptr);
}

bool __vmi_class_type_info::__do_upcast2(const __class_type_info *target, const void *obj_ptr,
                                         __upcast_result &result) const {
    if (__class_type_info::__do_upcast2(target, obj_ptr, result)) return true;

    const void *found_ptr = nullptr;
    int found_count = 0;
    bool is_public = true;
    for (unsigned int i = 0u; i < __base_count; i++) {
        __upcast_result sub = { nullptr, __not_contained, 0, nullptr };
        const void *sub_obj = adjust_base_ptr(obj_ptr, __base_info[i]);
        if (__base_info[i].__base_type->__do_upcast2(target, sub_obj, sub)) {
            if (!__base_info[i].__is_public_p() ||
                (sub.part2dst & __contained_public) != __contained_public) {
                is_public = false;
            }
            if (found_count == 0) {
                found_ptr = sub.dst_ptr;
                found_count = 1;
            } else if (found_ptr != sub.dst_ptr) {
                result.part2dst = __contained_ambig;
                return true;
            }
        }
    }
    if (found_count > 0) {
        result.dst_ptr = found_ptr;
        result.part2dst = is_public ? __contained_public : __contained_private;
        return true;
    }
    return false;
}

bool __vmi_class_type_info::__do_dyncast(ptrdiff_t src2dst, __sub_kind access_path,
                                         const __class_type_info *dst_type, const void *obj_ptr,
                                         const __class_type_info *src_type, const void *src_ptr,
                                         const void *&dst_ptr) const {
    if (*this == *dst_type && (access_path & __contained_public_mask)) {
        dst_ptr = obj_ptr;
        return true;
    }
    if (*this == *src_type && obj_ptr == src_ptr) {
        return false;
    }
    const void *found = nullptr;
    for (unsigned int i = 0u; i < __base_count; i++) {
        __sub_kind base_acc = __base_info[i].__is_public_p() ? access_path : __contained_private;
        const void *sub_obj = adjust_base_ptr(obj_ptr, __base_info[i]);
        const void *cand = nullptr;
        if (__base_info[i].__base_type->__do_dyncast(src2dst, base_acc, dst_type, sub_obj,
                                                     src_type, src_ptr, cand)) {
            if (found && found != cand) {
                return false; /* Ambiguous target */
            }
            found = cand;
        }
    }
    if (found) {
        dst_ptr = found;
        return true;
    }
    return false;
}

bool __pbase_type_info::__do_catch(const std::type_info *thr_type, void **thr_obj, unsigned outer) const {
    if (*this == *thr_type) return true;
    const __pbase_type_info *thr_p = dynamic_cast<const __pbase_type_info *>(thr_type);
    if (!thr_p) return false;
    if (__flags & __const_mask) outer |= 1u;
    if (outer & 1u) {
        if ((thr_p->__flags & ~__flags) & (__const_mask | __volatile_mask | __restrict_mask)) {
            return false;
        }
    } else if (thr_p->__flags != __flags) {
        return false;
    }
    if (__pointee && thr_p->__pointee && *__pointee == *thr_p->__pointee) {
        return true;
    }
    /* Allow Derived* -> void* or Derived* -> Base* when top-level pointer caught */
    if (thr_obj && __pointee && thr_p->__pointee) {
        if (strcmp(__pointee->name(), "v") == 0) {
            return true;
        }
        return __pointee->__do_catch(thr_p->__pointee, thr_obj, outer + 2u);
    }
    return false;
}

extern "C" void *__dynamic_cast(const void *sub,
                                const __class_type_info *src,
                                const __class_type_info *dst,
                                ptrdiff_t src2dst_offset) {
    (void)src2dst_offset;
    if (!sub || !src || !dst) return nullptr;

    const void *vtable_ptr = *static_cast<const void *const *>(sub);
    if (!vtable_ptr) return nullptr;
    const ptrdiff_t *prefix = static_cast<const ptrdiff_t *>(vtable_ptr);
    ptrdiff_t offset_to_top = prefix[-2];
    const __class_type_info *whole_type =
        reinterpret_cast<const __class_type_info *>(prefix[-1]);
    const void *whole_ptr = static_cast<const char *>(sub) + offset_to_top;

    if (!whole_type) return nullptr;
    if (*whole_type == *dst) {
        return const_cast<void *>(whole_ptr);
    }

    const void *dst_ptr = nullptr;
    if (whole_type->__do_dyncast(src2dst_offset, __class_type_info::__contained_public,
                                 dst, whole_ptr, src, sub, dst_ptr)) {
        return const_cast<void *>(dst_ptr);
    }
    return nullptr;
}

} // namespace __cxxabiv1

namespace std {

type_info::~type_info() {}
bool type_info::__is_pointer_p() const { return false; }
bool type_info::__is_function_p() const { return false; }
bool type_info::__do_catch(const type_info *thr_type, void **, unsigned) const {
    return *this == *thr_type;
}
bool type_info::__do_upcast(const __cxxabiv1::__class_type_info *, void **) const {
    return false;
}

exception::~exception() noexcept {}
const char* exception::what() const noexcept { return "std::exception"; }

bad_exception::~bad_exception() noexcept {}
const char* bad_exception::what() const noexcept { return "std::bad_exception"; }

bad_alloc::~bad_alloc() noexcept {}
const char* bad_alloc::what() const noexcept { return "std::bad_alloc"; }

bad_array_new_length::~bad_array_new_length() noexcept {}
const char* bad_array_new_length::what() const noexcept { return "std::bad_array_new_length"; }

bad_cast::~bad_cast() noexcept {}
const char* bad_cast::what() const noexcept { return "std::bad_cast"; }

bad_typeid::~bad_typeid() noexcept {}
const char* bad_typeid::what() const noexcept { return "std::bad_typeid"; }

} // namespace std

/* ---------------- Itanium C++ Exception Runtime ---------------- */

#define GNU_CXX_EXCEPTION_CLASS 0x474E5543432B2B00ULL /* "GNUCC++\0" */

struct __cxa_exception {
    const std::type_info *exceptionType;
    void (*exceptionDestructor)(void *);
    std::terminate_handler terminateHandler;
    struct __cxa_exception *nextException;
    int handlerCount;
    int handlerSwitchValue;
    const uint8_t *actionRecord;
    const uint8_t *languageSpecificData;
    const uint8_t *catchTemp;
    void *adjustedPtr;
    _Unwind_Exception unwindHeader;
};

struct __cxa_eh_globals {
    struct __cxa_exception *caughtExceptions;
    unsigned int uncaughtExceptions;
};

static pthread_once_t s_eh_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t s_eh_key;
static int s_eh_key_ok = 0;
static __cxa_eh_globals s_fallback_eh_globals = { nullptr, 0u };

static void init_eh_key(void) {
    if (pthread_key_create(&s_eh_key, free) == 0) {
        s_eh_key_ok = 1;
    }
}

extern "C" __cxa_eh_globals *__cxa_get_globals_fast(void) {
    if (!s_eh_key_ok) return &s_fallback_eh_globals;
    __cxa_eh_globals *g = (__cxa_eh_globals *)pthread_getspecific(s_eh_key);
    return g ? g : &s_fallback_eh_globals;
}

extern "C" __cxa_eh_globals *__cxa_get_globals(void) {
    pthread_once(&s_eh_key_once, init_eh_key);
    if (!s_eh_key_ok) return &s_fallback_eh_globals;
    __cxa_eh_globals *g = (__cxa_eh_globals *)pthread_getspecific(s_eh_key);
    if (!g) {
        g = (__cxa_eh_globals *)calloc(1u, sizeof(__cxa_eh_globals));
        if (!g) return &s_fallback_eh_globals;
        if (pthread_setspecific(s_eh_key, g) != 0) {
            free(g);
            return &s_fallback_eh_globals;
        }
    }
    return g;
}

namespace std {
bool uncaught_exception() noexcept {
    return __cxa_get_globals_fast()->uncaughtExceptions > 0u;
}
int uncaught_exceptions() noexcept {
    return (int)__cxa_get_globals_fast()->uncaughtExceptions;
}
} // namespace std

static inline __cxa_exception *cxa_exc_from_unwind(struct _Unwind_Exception *ue) {
    return ((__cxa_exception *)((uintptr_t)ue + sizeof(struct _Unwind_Exception))) - 1;
}

static void cxx_exception_cleanup(_Unwind_Reason_Code reason, struct _Unwind_Exception *ue) {
    (void)reason;
    __cxa_exception *hdr = cxa_exc_from_unwind(ue);
    void *obj = (void *)(hdr + 1);
    if (hdr->exceptionDestructor) {
        hdr->exceptionDestructor(obj);
    }
    free(hdr);
}

extern "C" {

void *__cxa_allocate_exception(size_t thrown_size) noexcept {
    size_t total = sizeof(__cxa_exception) + thrown_size + 16u;
    __cxa_exception *hdr = (__cxa_exception *)calloc(1u, total);
    if (!hdr) {
        std::terminate();
    }
    return (void *)(hdr + 1);
}

void __cxa_free_exception(void *thrown_exception) noexcept {
    if (!thrown_exception) return;
    __cxa_exception *hdr = ((__cxa_exception *)thrown_exception) - 1;
    free(hdr);
}

void __cxa_throw(void *thrown_exception, void *tinfo, void (*dest)(void *)) {
    __cxa_exception *hdr = ((__cxa_exception *)thrown_exception) - 1;
    hdr->exceptionType = (const std::type_info *)tinfo;
    hdr->exceptionDestructor = dest;
    hdr->terminateHandler = std::get_terminate();
    hdr->unwindHeader.exception_class = GNU_CXX_EXCEPTION_CLASS;
    hdr->unwindHeader.exception_cleanup = cxx_exception_cleanup;

    __cxa_eh_globals *g = __cxa_get_globals();
    g->uncaughtExceptions++;

    _Unwind_RaiseException(&hdr->unwindHeader);
    std::terminate();
}

void *__cxa_begin_catch(void *unwind_exc) {
    struct _Unwind_Exception *ue = (struct _Unwind_Exception *)unwind_exc;
    __cxa_exception *hdr = cxa_exc_from_unwind(ue);
    __cxa_eh_globals *g = __cxa_get_globals();

    if (hdr->handlerCount < 0) {
        hdr->handlerCount = -hdr->handlerCount;
    } else {
        hdr->nextException = g->caughtExceptions;
        g->caughtExceptions = hdr;
        hdr->handlerCount++;
    }
    if (g->uncaughtExceptions > 0u) {
        g->uncaughtExceptions--;
    }
    return hdr->adjustedPtr ? hdr->adjustedPtr : (void *)(hdr + 1);
}

void __cxa_end_catch(void) {
    __cxa_eh_globals *g = __cxa_get_globals_fast();
    __cxa_exception *hdr = g->caughtExceptions;
    if (!hdr) return;

    if (hdr->handlerCount < 0) {
        hdr->handlerCount++;
        if (hdr->handlerCount == 0) {
            g->caughtExceptions = hdr->nextException;
        }
        return;
    }

    hdr->handlerCount--;
    if (hdr->handlerCount == 0) {
        g->caughtExceptions = hdr->nextException;
        _Unwind_DeleteException(&hdr->unwindHeader);
    }
}

void __cxa_rethrow(void) {
    __cxa_eh_globals *g = __cxa_get_globals();
    __cxa_exception *hdr = g->caughtExceptions;
    if (!hdr) {
        std::terminate();
    }
    hdr->handlerCount = -hdr->handlerCount;
    hdr->unwindHeader.private_1 = 0u;
    hdr->unwindHeader.private_2 = 0u;
    g->uncaughtExceptions++;
    _Unwind_Resume_or_Rethrow(&hdr->unwindHeader);
    std::terminate();
}

const std::type_info *__cxa_current_exception_type(void) {
    __cxa_eh_globals *g = __cxa_get_globals_fast();
    return g->caughtExceptions ? g->caughtExceptions->exceptionType : nullptr;
}

void __cxa_bad_typeid(void) {
    throw std::bad_typeid();
}

void __cxa_bad_cast(void) {
    throw std::bad_cast();
}

void __cxa_throw_bad_array_new_length(void) {
    throw std::bad_array_new_length();
}

/* ---------------- GCC LSDA Personality Function ---------------- */

static uint32_t lsda_uleb128(const uint8_t **pp) {
    const uint8_t *p = *pp;
    uint32_t v = 0u, shift = 0u;
    uint8_t b;
    do {
        b = *p++;
        v |= ((uint32_t)(b & 0x7Fu)) << shift;
        shift += 7u;
    } while (b & 0x80u);
    *pp = p;
    return v;
}

static int32_t lsda_sleb128(const uint8_t **pp) {
    const uint8_t *p = *pp;
    uint32_t v = 0u, shift = 0u;
    uint8_t b;
    do {
        b = *p++;
        v |= ((uint32_t)(b & 0x7Fu)) << shift;
        shift += 7u;
    } while (b & 0x80u);
    if (shift < 32u && (b & 0x40u)) v |= (~0u << shift);
    *pp = p;
    return (int32_t)v;
}

static size_t lsda_enc_size(uint8_t enc) {
    switch (enc & 0x0Fu) {
    case 0x00u:
    case 0x03u:
    case 0x0Bu: return 4u;
    case 0x02u:
    case 0x0Au: return 2u;
    case 0x04u:
    case 0x0Cu: return 8u;
    default: return 4u;
    }
}

static uintptr_t lsda_read_ptr(const uint8_t **pp, uint8_t enc, uintptr_t base) {
    if (enc == 0xFFu) return 0u;
    const uint8_t *p = *pp;
    const uint8_t *addr = p;
    uint8_t fmt = enc & 0x0Fu;
    uint8_t app = enc & 0x70u;
    uint32_t raw = 0u;

    switch (fmt) {
    case 0x00u:
    case 0x03u:
        memcpy(&raw, p, 4u); p += 4; break;
    case 0x01u:
        raw = lsda_uleb128(&p); break;
    case 0x02u: {
        uint16_t v = 0u; memcpy(&v, p, 2u); raw = v; p += 2; break;
    }
    case 0x04u: {
        uint64_t v = 0ULL; memcpy(&v, p, 8u); raw = (uint32_t)v; p += 8; break;
    }
    case 0x09u:
        raw = (uint32_t)lsda_sleb128(&p); break;
    case 0x0Au: {
        int16_t v = 0; memcpy(&v, p, 2u); raw = (uint32_t)(int32_t)v; p += 2; break;
    }
    case 0x0Bu: {
        int32_t v = 0; memcpy(&v, p, 4u); raw = (uint32_t)v; p += 4; break;
    }
    case 0x0Cu: {
        int64_t v = 0; memcpy(&v, p, 8u); raw = (uint32_t)v; p += 8; break;
    }
    default: break;
    }
    *pp = p;

    if (raw == 0u && app != 0u) return 0u;
    uintptr_t res = 0u;
    switch (app) {
    case 0x00u: res = (uintptr_t)raw; break;
    case 0x10u: res = (uintptr_t)addr + (uintptr_t)raw; break;
    case 0x20u:
    case 0x30u:
    case 0x40u: res = base + (uintptr_t)raw; break;
    default:    res = (uintptr_t)raw; break;
    }
    if ((enc & 0x80u) && res != 0u) {
        uintptr_t ind = 0u;
        memcpy(&ind, (const void *)res, sizeof(ind));
        res = ind;
    }
    return res;
}

_Unwind_Reason_Code __gxx_personality_v0(int version,
                                         _Unwind_Action actions,
                                         uint64_t exception_class,
                                         struct _Unwind_Exception *ue,
                                         struct _Unwind_Context *ctx) {
    if (version != 1) return _URC_FATAL_PHASE1_ERROR;

    const uint8_t *lsda = (const uint8_t *)_Unwind_GetLanguageSpecificData(ctx);
    if (!lsda) return _URC_CONTINUE_UNWIND;

    uintptr_t func_start = _Unwind_GetRegionStart(ctx);
    int ip_before = 0;
    uintptr_t ip = _Unwind_GetIPInfo(ctx, &ip_before) - (ip_before ? 0u : 1u);

    const uint8_t *p = lsda;
    uint8_t lp_enc = *p++;
    uintptr_t lp_start = (lp_enc != 0xFFu) ? lsda_read_ptr(&p, lp_enc, (uintptr_t)lsda) : func_start;

    uint8_t ttype_enc = *p++;
    const uint8_t *ttype_end = nullptr;
    if (ttype_enc != 0xFFu) {
        uint32_t ttype_off = lsda_uleb128(&p);
        ttype_end = p + ttype_off;
    }

    uint8_t cs_enc = *p++;
    uint32_t cs_len = lsda_uleb128(&p);
    const uint8_t *cs_end = p + cs_len;
    const uint8_t *action_table = cs_end;

    uintptr_t landing_pad = 0u;
    uint32_t action_entry = 0u;
    int cs_matched = 0;

    while (p < cs_end) {
        uintptr_t cs_start = lsda_read_ptr(&p, cs_enc, (uintptr_t)lsda);
        uintptr_t cs_size = lsda_read_ptr(&p, cs_enc, (uintptr_t)lsda);
        uintptr_t cs_lp = lsda_read_ptr(&p, cs_enc, (uintptr_t)lsda);
        uint32_t cs_act = lsda_uleb128(&p);

        if (ip >= func_start + cs_start && ip < func_start + cs_start + cs_size) {
            if (cs_lp == 0u) {
                return _URC_CONTINUE_UNWIND;
            }
            landing_pad = lp_start + cs_lp;
            action_entry = cs_act;
            cs_matched = 1;
            break;
        }
    }

    if (!cs_matched) {
        return _URC_CONTINUE_UNWIND;
    }

    if (action_entry == 0u) {
        if (actions & _UA_SEARCH_PHASE) return _URC_CONTINUE_UNWIND;
        if (actions & _UA_CLEANUP_PHASE) {
            _Unwind_SetGR(ctx, 0, (uintptr_t)ue);
            _Unwind_SetGR(ctx, 2, 0u);
            _Unwind_SetIP(ctx, landing_pad);
            return _URC_INSTALL_CONTEXT;
        }
        return _URC_CONTINUE_UNWIND;
    }

    int is_foreign = (exception_class != GNU_CXX_EXCEPTION_CLASS) || (actions & _UA_FORCE_UNWIND);
    __cxa_exception *hdr = is_foreign ? nullptr : cxa_exc_from_unwind(ue);
    const std::type_info *thrown_ti = hdr ? hdr->exceptionType : nullptr;
    void *thrown_obj = hdr ? (void *)(hdr + 1) : nullptr;

    const uint8_t *act = action_table + (action_entry - 1u);
    int has_cleanup = 0;

    while (1) {
        int32_t ttype_filter = lsda_sleb128(&act);
        const uint8_t *next_field = act;
        int32_t next_off = lsda_sleb128(&act);

        if (ttype_filter == 0) {
            has_cleanup = 1;
        } else if (ttype_filter > 0 && ttype_end && !is_foreign) {
            size_t esz = lsda_enc_size(ttype_enc);
            const uint8_t *slot = ttype_end - (size_t)ttype_filter * esz;
            const std::type_info *catch_ti =
                (const std::type_info *)lsda_read_ptr(&slot, ttype_enc, (uintptr_t)lsda);
            void *adjusted = thrown_obj;
            bool caught = false;
            if (catch_ti == nullptr) {
                caught = true;
            } else if (thrown_ti && catch_ti->__do_catch(thrown_ti, &adjusted, 0u)) {
                caught = true;
            }
            if (caught) {
                if (actions & _UA_SEARCH_PHASE) {
                    hdr->adjustedPtr = adjusted;
                    return _URC_HANDLER_FOUND;
                }
                if (actions & _UA_HANDLER_FRAME) {
                    hdr->adjustedPtr = adjusted;
                    _Unwind_SetGR(ctx, 0, (uintptr_t)ue);
                    _Unwind_SetGR(ctx, 2, (uintptr_t)ttype_filter);
                    _Unwind_SetIP(ctx, landing_pad);
                    return _URC_INSTALL_CONTEXT;
                }
            }
        }

        if (next_off == 0) break;
        act = next_field + next_off;
    }

    if ((actions & _UA_CLEANUP_PHASE) && has_cleanup) {
        _Unwind_SetGR(ctx, 0, (uintptr_t)ue);
        _Unwind_SetGR(ctx, 2, 0u);
        _Unwind_SetIP(ctx, landing_pad);
        return _URC_INSTALL_CONTEXT;
    }

    return _URC_CONTINUE_UNWIND;
}

} // extern "C"
