#ifndef _IPO_CXX_UNWIND_H
#define _IPO_CXX_UNWIND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    _URC_NO_REASON = 0,
    _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
    _URC_FATAL_PHASE2_ERROR = 2,
    _URC_FATAL_PHASE1_ERROR = 3,
    _URC_NORMAL_STOP = 4,
    _URC_END_OF_STACK = 5,
    _URC_HANDLER_FOUND = 6,
    _URC_INSTALL_CONTEXT = 7,
    _URC_CONTINUE_UNWIND = 8
} _Unwind_Reason_Code;

typedef int _Unwind_Action;
#define _UA_SEARCH_PHASE  1
#define _UA_CLEANUP_PHASE 2
#define _UA_HANDLER_FRAME 4
#define _UA_FORCE_UNWIND  8
#define _UA_END_OF_STACK  16

struct _Unwind_Exception;
struct _Unwind_Context;

typedef void (*_Unwind_Exception_Cleanup_Fn)(_Unwind_Reason_Code reason,
                                             struct _Unwind_Exception *exc);

struct _Unwind_Exception {
    uint64_t exception_class;
    _Unwind_Exception_Cleanup_Fn exception_cleanup;
    uint32_t private_1;
    uint32_t private_2;
} __attribute__((__aligned__(8)));

typedef _Unwind_Reason_Code (*_Unwind_Personality_Fn)(
    int version,
    _Unwind_Action actions,
    uint64_t exception_class,
    struct _Unwind_Exception *exception_object,
    struct _Unwind_Context *context);

typedef _Unwind_Reason_Code (*_Unwind_Trace_Fn)(struct _Unwind_Context *context, void *arg);

_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *exc);
void _Unwind_Resume(struct _Unwind_Exception *exc) __attribute__((__noreturn__));
_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception *exc);
void _Unwind_DeleteException(struct _Unwind_Exception *exc);
_Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn trace, void *trace_argument);

uintptr_t _Unwind_GetGR(struct _Unwind_Context *context, int index);
void _Unwind_SetGR(struct _Unwind_Context *context, int index, uintptr_t new_value);
uintptr_t _Unwind_GetIP(struct _Unwind_Context *context);
uintptr_t _Unwind_GetIPInfo(struct _Unwind_Context *context, int *ip_before_insn);
void _Unwind_SetIP(struct _Unwind_Context *context, uintptr_t new_value);
uintptr_t _Unwind_GetCFA(struct _Unwind_Context *context);
void *_Unwind_GetLanguageSpecificData(struct _Unwind_Context *context);
uintptr_t _Unwind_GetRegionStart(struct _Unwind_Context *context);
uintptr_t _Unwind_GetDataRelBase(struct _Unwind_Context *context);
uintptr_t _Unwind_GetTextRelBase(struct _Unwind_Context *context);

void __register_frame_info(const void *begin, void *ob);
void __deregister_frame_info(const void *begin);

#ifdef __cplusplus
}
#endif

#endif /* _IPO_CXX_UNWIND_H */
