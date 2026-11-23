#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

void *operator new(std::size_t sz) { return std::calloc(1, sz ? sz : 1); }
void *operator new[](std::size_t sz) { return std::calloc(1, sz ? sz : 1); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace std {
exception::~exception() noexcept {}
const char *exception::what() const noexcept { return "std::exception"; }
type_info::~type_info() {}
void terminate() noexcept { std::abort(); }
} // namespace std

namespace {

struct AbiTypeInfo {
    const void *vptr;
    const char *name;
};

struct AbiSiTypeInfo {
    const void *vptr;
    const char *name;
    const AbiTypeInfo *base;
};

} // namespace

extern "C" {

void *__dso_handle = (void *)&__dso_handle;

void *_ZTVN10__cxxabiv117__class_type_infoE[4] = {nullptr, nullptr, nullptr, nullptr};
void *_ZTVN10__cxxabiv120__si_class_type_infoE[4] = {nullptr, nullptr, nullptr, nullptr};

void __cxa_pure_virtual(void) {
    std::abort();
}

void __cxa_bad_typeid(void) {
    std::abort();
}

int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
    (void)func; (void)arg; (void)dso;
    return 0;
}

int __cxa_guard_acquire(uint64_t *guard) {
    volatile uint8_t *p = reinterpret_cast<volatile uint8_t *>(guard);
    return *p == 0 ? 1 : 0;
}

void __cxa_guard_release(uint64_t *guard) {
    volatile uint8_t *p = reinterpret_cast<volatile uint8_t *>(guard);
    *p = 1;
}

void __cxa_guard_abort(uint64_t *guard) {
    (void)guard;
}

int __gxx_personality_v0(int version, int actions, uint64_t exc_class,
                         void *exc_obj, void *ctx) {
    (void)version; (void)actions; (void)exc_class; (void)exc_obj; (void)ctx;
    return 8;
}

void _Unwind_Resume(void *exc_obj) {
    (void)exc_obj;
    std::abort();
}

void *__cxa_allocate_exception(std::size_t size) {
    return std::calloc(1, size + 64);
}

void __cxa_free_exception(void *ptr) {
    std::free(ptr);
}

void __cxa_throw(void *exc, void *tinfo, void (*dest)(void *)) {
    (void)exc; (void)tinfo; (void)dest;
    std::abort();
}

void *__cxa_begin_catch(void *exc) {
    return exc;
}

void __cxa_end_catch(void) {}

void *__dynamic_cast(const void *sub,
                     const AbiTypeInfo *src,
                     const AbiTypeInfo *dst,
                     std::ptrdiff_t src2dst_offset) {
    (void)src;
    (void)src2dst_offset;
    if (!sub || !dst) return nullptr;

    const void *const *vptr = *reinterpret_cast<const void *const *const *>(sub);
    if (!vptr) return nullptr;

    std::ptrdiff_t offset_to_top = reinterpret_cast<std::ptrdiff_t>(vptr[-2]);
    const void *complete_obj = reinterpret_cast<const char *>(sub) + offset_to_top;
    const AbiTypeInfo *cur = reinterpret_cast<const AbiTypeInfo *>(vptr[-1]);

    const void *si_vptr = &_ZTVN10__cxxabiv120__si_class_type_infoE[2];
    for (int depth = 0; cur && depth < 16; ++depth) {
        if (cur == dst || (cur->name && dst->name && std::strcmp(cur->name, dst->name) == 0)) {
            return const_cast<void *>(complete_obj);
        }
        if (cur->vptr == si_vptr) {
            cur = reinterpret_cast<const AbiSiTypeInfo *>(cur)->base;
        } else {
            break;
        }
    }
    return nullptr;
}

} // extern "C"
