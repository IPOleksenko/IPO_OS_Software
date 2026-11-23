#ifndef LINKS_IPO_JS_ENGINE_H
#define LINKS_IPO_JS_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ipo_js_vm ipo_js_vm_t;
typedef struct ipo_js_str ipo_js_str_t;
typedef struct ipo_js_obj ipo_js_obj_t;
typedef struct ipo_js_env ipo_js_env_t;
typedef struct ipo_js_ast ipo_js_ast_t;

typedef enum {
	JS_T_UNDEFINED = 0,
	JS_T_NULL,
	JS_T_BOOL,
	JS_T_NUMBER,
	JS_T_STRING,
	JS_T_OBJECT
} ipo_js_type_t;

typedef enum {
	JS_OBJ_PLAIN = 0,
	JS_OBJ_ARRAY,
	JS_OBJ_FUNCTION,
	JS_OBJ_NATIVE_FN,
	JS_OBJ_REGEXP,
	JS_OBJ_DATE,
	JS_OBJ_ERROR,
	JS_OBJ_PROMISE
} ipo_js_obj_kind_t;

typedef struct ipo_js_val {
	ipo_js_type_t type;
	union {
		int b;
		double num;
		ipo_js_str_t *str;
		ipo_js_obj_t *obj;
	} u;
} ipo_js_val_t;

typedef ipo_js_val_t (*ipo_js_native_fn_t)(ipo_js_vm_t *vm, ipo_js_val_t this_val,
                                           int argc, const ipo_js_val_t *argv, void *userdata);

typedef int (*ipo_js_host_get_fn)(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key,
                                  ipo_js_val_t *out_val);
typedef int (*ipo_js_host_set_fn)(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key,
                                  ipo_js_val_t val);
typedef void (*ipo_js_host_finalize_fn)(ipo_js_vm_t *vm, ipo_js_obj_t *obj);
typedef void (*ipo_js_host_mark_fn)(ipo_js_vm_t *vm, ipo_js_obj_t *obj);

struct ipo_js_str {
	uint8_t marked;
	size_t len;
	uint32_t hash;
	struct ipo_js_str *gc_next;
	char data[1];
};

typedef struct ipo_js_prop {
	ipo_js_str_t *key;
	ipo_js_val_t val;
	ipo_js_obj_t *getter;
	ipo_js_obj_t *setter;
	uint8_t flags; /* bit 0: enumerable, bit 1: writable, bit 2: accessor */
} ipo_js_prop_t;

struct ipo_js_obj {
	ipo_js_obj_kind_t kind;
	uint8_t marked;
	uint32_t pin_count;
	ipo_js_obj_t *proto;
	ipo_js_prop_t *props;
	size_t prop_len;
	size_t prop_cap;

	/* Native / host object hooks */
	int host_tag;
	void *host_ptr;
	ipo_js_host_get_fn host_get;
	ipo_js_host_set_fn host_set;
	ipo_js_host_finalize_fn host_finalize;
	ipo_js_host_mark_fn host_mark;

	/* Kind-specific payload */
	union {
		struct {
			ipo_js_val_t *items;
			size_t len;
			size_t cap;
		} arr;
		struct {
			ipo_js_ast_t *func_ast;
			ipo_js_ast_t *cls_ast;
			ipo_js_env_t *closure;
			ipo_js_str_t *name;
			int is_arrow;
			int is_bound;
			ipo_js_val_t bound_this;
			ipo_js_val_t *bound_args;
			int bound_argc;
		} fn;
		struct {
			ipo_js_native_fn_t fn;
			void *userdata;
			ipo_js_str_t *name;
			int is_ctor;
			int is_bound;
			ipo_js_val_t bound_this;
			ipo_js_val_t *bound_args;
			int bound_argc;
		} nat;
		struct {
			ipo_js_str_t *pattern;
			uint8_t flags; /* bit0:i, bit1:g, bit2:m, bit3:s */
			size_t last_index;
		} re;
		struct {
			double ms;
		} date;
		struct {
			int state; /* 0=pending, 1=fulfilled, 2=rejected */
			ipo_js_val_t result;
			ipo_js_val_t *then_fns;
			ipo_js_val_t *catch_fns;
			ipo_js_val_t *then_promises;
			int cb_len;
			int cb_cap;
		} prom;
	} u;

	ipo_js_obj_t *gc_next;
};

/* VM Lifecycle */
ipo_js_vm_t *ipo_js_vm_new(void *host_ctx, int max_call_depth, int max_steps);
void ipo_js_vm_free(ipo_js_vm_t *vm);
void *ipo_js_vm_host_ctx(ipo_js_vm_t *vm);
void ipo_js_vm_set_limits(ipo_js_vm_t *vm, int max_call_depth, int max_steps);

/* Value constructors */
ipo_js_val_t ipo_js_undef(void);
ipo_js_val_t ipo_js_null(void);
ipo_js_val_t ipo_js_bool(int b);
ipo_js_val_t ipo_js_num(double d);
ipo_js_val_t ipo_js_str_new(ipo_js_vm_t *vm, const char *s, size_t len);
ipo_js_val_t ipo_js_str_cstr(ipo_js_vm_t *vm, const char *s);
ipo_js_val_t ipo_js_obj_val(ipo_js_obj_t *o);

/* Object / Function / Array helpers */
ipo_js_obj_t *ipo_js_new_object(ipo_js_vm_t *vm, ipo_js_obj_t *proto);
ipo_js_obj_t *ipo_js_new_array(ipo_js_vm_t *vm, size_t initial_len);
ipo_js_obj_t *ipo_js_new_native_fn(ipo_js_vm_t *vm, const char *name,
                                   ipo_js_native_fn_t fn, void *userdata, int is_ctor);
ipo_js_obj_t *ipo_js_new_error(ipo_js_vm_t *vm, const char *name, const char *msg);
ipo_js_obj_t *ipo_js_new_promise(ipo_js_vm_t *vm);
void ipo_js_promise_resolve(ipo_js_vm_t *vm, ipo_js_obj_t *p, ipo_js_val_t val);
void ipo_js_promise_reject(ipo_js_vm_t *vm, ipo_js_obj_t *p, ipo_js_val_t err);

/* Global / Property / Array access */
ipo_js_obj_t *ipo_js_global(ipo_js_vm_t *vm);
ipo_js_obj_t *ipo_js_object_proto(ipo_js_vm_t *vm);
ipo_js_obj_t *ipo_js_array_proto(ipo_js_vm_t *vm);
int  ipo_js_set_prop(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t val);
int  ipo_js_get_prop(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t *out);
int  ipo_js_has_own_prop(ipo_js_obj_t *obj, const char *key);
int  ipo_js_del_prop(ipo_js_obj_t *obj, const char *key);
void ipo_js_arr_push(ipo_js_vm_t *vm, ipo_js_obj_t *arr, ipo_js_val_t val);
int  ipo_js_is_callable(ipo_js_val_t v);

/* Conversions & Formatting */
int    ipo_js_to_bool(ipo_js_val_t v);
double ipo_js_to_num(ipo_js_vm_t *vm, ipo_js_val_t v);
int32_t ipo_js_to_i32(ipo_js_vm_t *vm, ipo_js_val_t v);
ipo_js_str_t *ipo_js_to_str(ipo_js_vm_t *vm, ipo_js_val_t v);
const char *ipo_js_cstr(ipo_js_vm_t *vm, ipo_js_val_t v, size_t *out_len);

/* Execution / Evaluation / Calls / GC */
int ipo_js_eval(ipo_js_vm_t *vm, const char *code, size_t len, const char *filename,
                ipo_js_val_t *out_val);
int ipo_js_call(ipo_js_vm_t *vm, ipo_js_val_t fn, ipo_js_val_t this_val,
                int argc, const ipo_js_val_t *argv, ipo_js_val_t *out_val);
void ipo_js_vm_queue_microtask(ipo_js_vm_t *vm, ipo_js_val_t fn);
void ipo_js_throw(ipo_js_vm_t *vm, ipo_js_val_t err);
void ipo_js_throw_msg(ipo_js_vm_t *vm, const char *err_type, const char *fmt, ...);
int  ipo_js_has_exception(ipo_js_vm_t *vm);
ipo_js_val_t ipo_js_get_exception(ipo_js_vm_t *vm);
void ipo_js_clear_exception(ipo_js_vm_t *vm);

/* JSON */
int ipo_js_json_parse(ipo_js_vm_t *vm, const char *s, size_t len, ipo_js_val_t *out);
ipo_js_str_t *ipo_js_json_stringify(ipo_js_vm_t *vm, ipo_js_val_t val, int indent);

/* GC protection */
void ipo_js_gc_pin(ipo_js_obj_t *obj);
void ipo_js_gc_unpin(ipo_js_obj_t *obj);
void ipo_js_gc_mark_val(ipo_js_vm_t *vm, ipo_js_val_t v);
void ipo_js_gc_mark_obj(ipo_js_vm_t *vm, ipo_js_obj_t *o);
void ipo_js_gc_mark_str(ipo_js_vm_t *vm, ipo_js_str_t *s);
size_t ipo_js_gc_push_root(ipo_js_vm_t *vm, ipo_js_val_t v);
void ipo_js_gc_pop_roots(ipo_js_vm_t *vm, size_t saved_top);
void ipo_js_gc_collect(ipo_js_vm_t *vm);
size_t ipo_js_vm_mem_used(ipo_js_vm_t *vm);

/* Optional host hooks configured by Links runtime */
typedef void (*ipo_js_console_fn)(void *host_ctx, int level, const char *msg);
void ipo_js_vm_set_console_hook(ipo_js_vm_t *vm, ipo_js_console_fn fn);

/* Bridge helpers exposed between ipo_js.c and ipo_gui.c */
int ipo_gui_run_js_dialog(int kind, const char *title, const char *message,
                          const char *default_text, char **out_alloc);
void ipo_gui_notify_js_console(const char *msg);
int  ipo_gui_dom_dirty_hint(void);

#ifdef __cplusplus
}
#endif

#endif
