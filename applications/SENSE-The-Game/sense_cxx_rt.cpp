/*
 * Minimal freestanding C++17 runtime support for SENSE: The Game on IPO_OS.
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <typeinfo>

void* operator new(std::size_t size) {
    return std::malloc(size ? size : 1);
}

void* operator new[](std::size_t size) {
    return std::malloc(size ? size : 1);
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

namespace std {
exception::~exception() noexcept {}
const char* exception::what() const noexcept { return "std::exception"; }
type_info::~type_info() {}
void terminate() noexcept {
    std::abort();
}
} // namespace std

extern "C" {

void* __dso_handle = (void*)&__dso_handle;

void* _ZTVN10__cxxabiv117__class_type_infoE[4] = { nullptr, nullptr, nullptr, nullptr };
void* _ZTVN10__cxxabiv120__si_class_type_infoE[4] = { nullptr, nullptr, nullptr, nullptr };

void __cxa_pure_virtual(void) {
    std::fprintf(stderr, "[SENSE: The Game] pure virtual call\n");
    std::abort();
}

struct sense_cxa_atexit_entry {
    void (*func)(void*);
    void* arg;
    void* dso;
};

static sense_cxa_atexit_entry* s_atexit = nullptr;
static std::size_t s_atexit_count = 0;
static std::size_t s_atexit_cap = 0;

int __cxa_atexit(void (*func)(void*), void* arg, void* dso) {
    if (!func) return -1;
    if (s_atexit_count >= s_atexit_cap) {
        std::size_t ncap = s_atexit_cap ? s_atexit_cap * 2 : 32;
        auto* nbuf = static_cast<sense_cxa_atexit_entry*>(
            std::realloc(s_atexit, ncap * sizeof(sense_cxa_atexit_entry)));
        if (!nbuf) return -1;
        s_atexit = nbuf;
        s_atexit_cap = ncap;
    }
    s_atexit[s_atexit_count++] = {func, arg, dso};
    return 0;
}

void __cxa_finalize(void* dso) {
    for (int i = static_cast<int>(s_atexit_count) - 1; i >= 0; --i) {
        if (s_atexit[i].func && (!dso || s_atexit[i].dso == dso)) {
            void (*f)(void*) = s_atexit[i].func;
            void* a = s_atexit[i].arg;
            s_atexit[i].func = nullptr;
            f(a);
        }
    }
    if (!dso && s_atexit) {
        std::free(s_atexit);
        s_atexit = nullptr;
        s_atexit_count = 0;
        s_atexit_cap = 0;
    }
}

int __cxa_guard_acquire(uint64_t* guard) {
    volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(guard);
    return *p == 0 ? 1 : 0;
}

void __cxa_guard_release(uint64_t* guard) {
    volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(guard);
    *p = 1;
}

void __cxa_guard_abort(uint64_t* guard) {
    (void)guard;
}

int __gxx_personality_v0(int version, int actions, uint64_t exc_class,
                         void* exc_obj, void* ctx) {
    (void)version; (void)actions; (void)exc_class; (void)exc_obj; (void)ctx;
    return 8; /* _URC_CONTINUE_UNWIND */
}

void _Unwind_Resume(void* exc_obj) {
    (void)exc_obj;
    std::abort();
}

void* __cxa_allocate_exception(std::size_t size) {
    return std::calloc(1, size + 64);
}

void __cxa_free_exception(void* ptr) {
    std::free(ptr);
}

void __cxa_throw(void* exc, void* tinfo, void (*dest)(void*)) {
    (void)exc; (void)tinfo; (void)dest;
    std::abort();
}

void* __cxa_begin_catch(void* exc) {
    return exc;
}

void __cxa_end_catch(void) {
}

} // extern "C"
