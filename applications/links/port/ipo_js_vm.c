/*
 * Standalone ES5/ES6+ JavaScript Virtual Machine for Links Browser (IPO_OS)
 * Self-contained C11 lexer, recursive-descent AST parser, closure evaluator,
 * mark-and-sweep GC, RegExp NFA matcher, JSON, and ES standard library.
 */

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "ipo_js_engine.h"

/* =========================================================================
 * Allocation Helpers & GC
 * ========================================================================= */

static void *xmalloc(size_t sz) {
	void *p = malloc(sz ? sz : 1);
	return p;
}

static void *xcalloc(size_t n, size_t sz) {
	void *p = calloc(n ? n : 1, sz ? sz : 1);
	return p;
}

static void *xrealloc(void *p, size_t sz) {
	return realloc(p, sz ? sz : 1);
}

static char *xstrdup_n(const char *s, size_t len) {
	char *r = (char *)xmalloc(len + 1);
	if (r) {
		if (len && s) memcpy(r, s, len);
		r[len] = '\0';
	}
	return r;
}

static uint32_t fnv1a_hash(const char *s, size_t len) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < len; i++) {
		h ^= (uint8_t)s[i];
		h *= 16777619u;
	}
	return h;
}

typedef struct js_buf {
	char *data;
	size_t len;
	size_t cap;
} js_buf_t;

static void jbuf_init(js_buf_t *b) {
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static void jbuf_free(js_buf_t *b) {
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static void jbuf_reserve(js_buf_t *b, size_t need) {
	if (need <= b->cap) return;
	size_t ncap = b->cap ? b->cap : 64;
	while (ncap < need) ncap *= 2;
	char *nd = (char *)xrealloc(b->data, ncap);
	if (nd) {
		b->data = nd;
		b->cap = ncap;
	}
}

static void jbuf_append(js_buf_t *b, const char *s, size_t n) {
	if (!s && n) return;
	jbuf_reserve(b, b->len + n + 1);
	if (b->data) {
		if (n) memcpy(b->data + b->len, s, n);
		b->len += n;
		b->data[b->len] = '\0';
	}
}

static void jbuf_append_ch(js_buf_t *b, char c) {
	jbuf_append(b, &c, 1);
}

static void jbuf_append_cstr(js_buf_t *b, const char *s) {
	if (s) jbuf_append(b, s, strlen(s));
}

static void jbuf_append_utf8(js_buf_t *b, uint32_t cp) {
	char tmp[4];
	size_t n = 0;
	if (cp <= 0x7F) {
		tmp[0] = (char)cp;
		n = 1;
	} else if (cp <= 0x7FF) {
		tmp[0] = (char)(0xC0 | ((cp >> 6) & 0x1F));
		tmp[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp <= 0xFFFF) {
		tmp[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
		tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else if (cp <= 0x10FFFF) {
		tmp[0] = (char)(0xF0 | ((cp >> 18) & 0x07));
		tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	} else {
		tmp[0] = '?';
		n = 1;
	}
	jbuf_append(b, tmp, n);
}

/* =========================================================================
 * Environment, Frame, Microtask, AST & VM Definitions
 * ========================================================================= */

typedef struct js_binding {
	ipo_js_str_t *name;
	ipo_js_val_t val;
	uint8_t is_const;
} js_binding_t;

struct ipo_js_env {
	uint8_t marked;
	uint8_t is_func_scope;
	ipo_js_env_t *parent;
	js_binding_t *vars;
	size_t len;
	size_t cap;
	int has_this;
	ipo_js_val_t this_val;
	ipo_js_obj_t *with_obj; /* non-NULL for global env mapped to global_obj */
	ipo_js_env_t *gc_next;
};

typedef struct js_ast_list {
	ipo_js_ast_t **items;
	size_t len;
	size_t cap;
} js_ast_list_t;

typedef struct js_case_clause {
	ipo_js_ast_t *test; /* NULL for default: */
	js_ast_list_t body;
} js_case_clause_t;

typedef struct js_obj_prop_ast {
	ipo_js_ast_t *key_ast; /* if computed */
	char *key_name;        /* if non-computed */
	ipo_js_ast_t *val;
	int kind; /* 0=normal, 1=get, 2=set, 3=spread */
} js_obj_prop_ast_t;

typedef struct js_param_ast {
	ipo_js_ast_t *pattern; /* AST_IDENT, AST_ARR_PAT, AST_OBJ_PAT */
	ipo_js_ast_t *def_val;
	int is_rest;
} js_param_ast_t;

typedef enum {
	AST_EMPTY = 0,
	AST_BLOCK,
	AST_VAR_DECL,
	AST_EXPR_STMT,
	AST_IF,
	AST_WHILE,
	AST_DO_WHILE,
	AST_FOR,
	AST_FOR_IN,
	AST_FOR_OF,
	AST_SWITCH,
	AST_BREAK,
	AST_CONTINUE,
	AST_RETURN,
	AST_THROW,
	AST_TRY,
	AST_LABEL,
	AST_FUNC,
	AST_CLASS,

	AST_UNDEF,
	AST_NULL,
	AST_BOOL,
	AST_NUM,
	AST_STR,
	AST_REGEXP,
	AST_TEMPLATE,
	AST_THIS,
	AST_SUPER,
	AST_IDENT,
	AST_ARRAY,
	AST_OBJECT,
	AST_ARROW,
	AST_SPREAD,
	AST_ARR_PAT,
	AST_OBJ_PAT,

	AST_MEMBER,
	AST_CALL,
	AST_NEW,
	AST_UNARY,
	AST_POSTFIX,
	AST_BINARY,
	AST_ASSIGN,
	AST_TERNARY,
	AST_SEQ
} js_ast_kind_t;

typedef struct js_pat_prop {
	char *key_name;
	ipo_js_ast_t *key_ast;
	ipo_js_ast_t *target;
	ipo_js_ast_t *def_val;
	int is_rest;
} js_pat_prop_t;

typedef struct js_class_elem {
	char *name;
	ipo_js_ast_t *key_ast;
	ipo_js_ast_t *fn_ast;
	int is_static;
	int kind; /* 0=method, 1=get, 2=set */
} js_class_elem_t;

struct ipo_js_ast {
	js_ast_kind_t kind;
	int line;
	int col;
	union {
		struct { js_ast_list_t stmts; } block;
		struct {
			int decl_kind; /* 0=var, 1=let, 2=const */
			ipo_js_ast_t **targets;
			ipo_js_ast_t **inits;
			size_t len;
		} var_decl;
		struct { ipo_js_ast_t *expr; } expr_stmt;
		struct { ipo_js_ast_t *test, *cons, *alt; } if_s;
		struct { ipo_js_ast_t *test, *body; } loop_s;
		struct { ipo_js_ast_t *init, *test, *upd, *body; } for_s;
		struct {
			int decl_kind; /* -1 = lvalue expr, 0=var, 1=let, 2=const */
			ipo_js_ast_t *lhs, *rhs, *body;
		} for_iter;
		struct {
			ipo_js_ast_t *disc;
			js_case_clause_t *cases;
			size_t len;
		} sw;
		struct { char *label; } jump;
		struct { char *label; ipo_js_ast_t *stmt; } labeled;
		struct {
			ipo_js_ast_t *block;
			char *catch_name;
			ipo_js_ast_t *catch_pat;
			ipo_js_ast_t *catch_block;
			ipo_js_ast_t *finally_block;
		} try_s;
		struct {
			char *name;
			js_param_ast_t *params;
			size_t param_len;
			ipo_js_ast_t *body;
			int is_decl;
			int is_arrow;
			int is_async;
		} fn;
		struct {
			char *name;
			ipo_js_ast_t *super_expr;
			js_class_elem_t *elems;
			size_t elem_len;
			int is_decl;
		} cls;

		struct { int b; } b_lit;
		struct { double num; } n_lit;
		struct { char *str; size_t len; } s_lit;
		struct { char *pat; char *flags; } re_lit;
		struct {
			char **quasis;
			size_t *qlen;
			ipo_js_ast_t **exprs;
			size_t count; /* quasis = count + 1, exprs = count */
		} tmpl;
		struct { char *name; } ident;
		struct { js_ast_list_t elems; } arr;
		struct { js_obj_prop_ast_t *props; size_t len; } obj;
		struct { ipo_js_ast_t *arg; } spread;
		struct { js_pat_prop_t *elems; size_t len; } pat;
		struct {
			ipo_js_ast_t *obj, *prop_expr;
			char *prop_name;
			int computed;
			int optional;
		} mem;
		struct {
			ipo_js_ast_t *callee;
			js_ast_list_t args;
			int optional;
		} call;
		struct {
			int op;
			ipo_js_ast_t *arg;
		} unary;
		struct {
			int op;
			ipo_js_ast_t *left, *right;
		} bin;
		struct { ipo_js_ast_t *test, *cons, *alt; } tern;
		struct { js_ast_list_t exprs; } seq;
	} u;
};

typedef struct js_ast_pool {
	ipo_js_ast_t **nodes;
	size_t len;
	size_t cap;
} js_ast_pool_t;

typedef enum {
	SIG_NONE = 0,
	SIG_RETURN,
	SIG_BREAK,
	SIG_CONTINUE,
	SIG_THROW
} js_sig_t;

typedef struct js_microtask {
	ipo_js_val_t fn;
	ipo_js_val_t arg;
	ipo_js_obj_t *target_prom;
	int is_reject;
} js_microtask_t;

typedef struct js_call_frame {
	ipo_js_env_t *env;
	ipo_js_val_t this_val;
	ipo_js_obj_t *super_proto;
	ipo_js_obj_t *active_fn;
} js_call_frame_t;

struct ipo_js_vm {
	void *host_ctx;
	ipo_js_console_fn console_hook;

	int max_depth;
	int max_steps;
	int call_depth;
	int eval_depth;
	int ast_depth;
	int mark_depth;
	int steps;

	/* Exception & Control Flow */
	js_sig_t sig;
	ipo_js_val_t sig_val;
	char *sig_label;
	int has_exc;
	ipo_js_val_t exc_val;

	/* Globals & Built-in Prototypes */
	ipo_js_obj_t *global_obj;
	ipo_js_env_t *global_env;
	ipo_js_obj_t *obj_proto;
	ipo_js_obj_t *arr_proto;
	ipo_js_obj_t *str_proto;
	ipo_js_obj_t *fn_proto;
	ipo_js_obj_t *err_proto;
	ipo_js_obj_t *regexp_proto;
	ipo_js_obj_t *date_proto;
	ipo_js_obj_t *promise_proto;

	/* GC lists & root stack */
	ipo_js_str_t *gc_strs;
	ipo_js_obj_t *gc_objs;
	ipo_js_env_t *gc_envs;
	size_t gc_bytes;
	size_t gc_threshold;

	ipo_js_val_t *root_stack;
	size_t root_len;
	size_t root_cap;

	ipo_js_obj_t **mark_stack;
	size_t mark_len;
	size_t mark_cap;

	js_call_frame_t *frames;
	int frame_len;
	int frame_cap;

	/* Microtask queue for Promises */
	js_microtask_t *microtasks;
	int mt_head;
	int mt_len;
	int mt_cap;
	int draining_mt;

	/* Persistent parsed AST pools */
	js_ast_pool_t **ast_pools;
	size_t ast_pool_len;

	uint32_t rng_state;
};

/* Forward declarations */
static ipo_js_val_t vm_exec_stmt(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *ast);
static ipo_js_val_t vm_eval_expr(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *ast);
static ipo_js_val_t vm_call_obj(ipo_js_vm_t *vm, ipo_js_val_t callee, ipo_js_val_t this_val,
                                int argc, const ipo_js_val_t *argv, ipo_js_obj_t *super_proto);
static ipo_js_val_t vm_construct_obj(ipo_js_vm_t *vm, ipo_js_val_t ctor,
                                     int argc, const ipo_js_val_t *argv);
static int vm_get_prop_val(ipo_js_vm_t *vm, ipo_js_val_t obj_v, const char *key, ipo_js_val_t *out);
static int vm_set_prop_val(ipo_js_vm_t *vm, ipo_js_val_t obj_v, const char *key, ipo_js_val_t val);
static int js_strict_eq(ipo_js_val_t a, ipo_js_val_t b);
static int js_loose_eq(ipo_js_vm_t *vm, ipo_js_val_t a, ipo_js_val_t b);
static ipo_js_obj_t *vm_new_regexp(ipo_js_vm_t *vm, const char *pat, size_t plen, uint8_t flags);
static void vm_drain_microtasks(ipo_js_vm_t *vm);

/* =========================================================================
 * Value Constructors & GC Implementation
 * ========================================================================= */

ipo_js_val_t ipo_js_undef(void) {
	ipo_js_val_t v;
	v.type = JS_T_UNDEFINED;
	v.u.num = 0;
	return v;
}

ipo_js_val_t ipo_js_null(void) {
	ipo_js_val_t v;
	v.type = JS_T_NULL;
	v.u.num = 0;
	return v;
}

ipo_js_val_t ipo_js_bool(int b) {
	ipo_js_val_t v;
	v.type = JS_T_BOOL;
	v.u.b = b ? 1 : 0;
	return v;
}

ipo_js_val_t ipo_js_num(double d) {
	ipo_js_val_t v;
	v.type = JS_T_NUMBER;
	v.u.num = d;
	return v;
}

ipo_js_val_t ipo_js_obj_val(ipo_js_obj_t *o) {
	ipo_js_val_t v;
	if (!o) return ipo_js_null();
	v.type = JS_T_OBJECT;
	v.u.obj = o;
	return v;
}

size_t ipo_js_gc_push_root(ipo_js_vm_t *vm, ipo_js_val_t v) {
	size_t prev = vm->root_len;
	if (vm->root_len >= vm->root_cap) {
		size_t ncap = vm->root_cap ? vm->root_cap * 2 : 64;
		ipo_js_val_t *nr = (ipo_js_val_t *)xrealloc(vm->root_stack, ncap * sizeof(ipo_js_val_t));
		if (!nr) return prev;
		vm->root_stack = nr;
		vm->root_cap = ncap;
	}
	vm->root_stack[vm->root_len++] = v;
	return prev;
}

void ipo_js_gc_pop_roots(ipo_js_vm_t *vm, size_t saved_top) {
	if (saved_top <= vm->root_len) vm->root_len = saved_top;
}

static ipo_js_str_t *vm_alloc_str(ipo_js_vm_t *vm, const char *s, size_t len) {
	size_t bytes = sizeof(ipo_js_str_t) + len;
	ipo_js_str_t *st = (ipo_js_str_t *)xmalloc(bytes);
	if (!st) return NULL;
	st->marked = 0;
	st->len = len;
	if (len && s) memcpy(st->data, s, len);
	st->data[len] = '\0';
	st->hash = fnv1a_hash(st->data, len);
	st->gc_next = vm->gc_strs;
	vm->gc_strs = st;
	vm->gc_bytes += bytes;
	return st;
}

ipo_js_val_t ipo_js_str_new(ipo_js_vm_t *vm, const char *s, size_t len) {
	ipo_js_val_t v;
	v.type = JS_T_STRING;
	v.u.str = vm_alloc_str(vm, s ? s : "", len);
	return v;
}

ipo_js_val_t ipo_js_str_cstr(ipo_js_vm_t *vm, const char *s) {
	if (!s) s = "";
	return ipo_js_str_new(vm, s, strlen(s));
}

static ipo_js_obj_t *vm_alloc_obj(ipo_js_vm_t *vm, ipo_js_obj_kind_t kind, ipo_js_obj_t *proto) {
	ipo_js_obj_t *o = (ipo_js_obj_t *)xcalloc(1, sizeof(ipo_js_obj_t));
	if (!o) return NULL;
	o->kind = kind;
	o->proto = proto;
	o->gc_next = vm->gc_objs;
	vm->gc_objs = o;
	vm->gc_bytes += sizeof(ipo_js_obj_t);
	return o;
}

ipo_js_obj_t *ipo_js_new_object(ipo_js_vm_t *vm, ipo_js_obj_t *proto) {
	return vm_alloc_obj(vm, JS_OBJ_PLAIN, proto ? proto : vm->obj_proto);
}

ipo_js_obj_t *ipo_js_new_array(ipo_js_vm_t *vm, size_t initial_len) {
	ipo_js_obj_t *o = vm_alloc_obj(vm, JS_OBJ_ARRAY, vm->arr_proto);
	if (!o) return NULL;
	if (initial_len > 0) {
		o->u.arr.items = (ipo_js_val_t *)xmalloc(initial_len * sizeof(ipo_js_val_t));
		if (o->u.arr.items) {
			o->u.arr.len = initial_len;
			o->u.arr.cap = initial_len;
			for (size_t i = 0; i < initial_len; i++) o->u.arr.items[i] = ipo_js_undef();
			vm->gc_bytes += initial_len * sizeof(ipo_js_val_t);
		}
	}
	return o;
}

ipo_js_obj_t *ipo_js_new_native_fn(ipo_js_vm_t *vm, const char *name,
                                   ipo_js_native_fn_t fn, void *userdata, int is_ctor) {
	ipo_js_obj_t *o = vm_alloc_obj(vm, JS_OBJ_NATIVE_FN, vm->fn_proto);
	if (!o) return NULL;
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(o));
	o->u.nat.fn = fn;
	o->u.nat.userdata = userdata;
	o->u.nat.is_ctor = is_ctor;
	o->u.nat.name = name ? vm_alloc_str(vm, name, strlen(name)) : vm_alloc_str(vm, "", 0);
	if (is_ctor) {
		ipo_js_obj_t *p = ipo_js_new_object(vm, vm->obj_proto);
		ipo_js_set_prop(vm, o, "prototype", ipo_js_obj_val(p));
		ipo_js_set_prop(vm, p, "constructor", ipo_js_obj_val(o));
	}
	ipo_js_gc_pop_roots(vm, top);
	return o;
}

ipo_js_obj_t *ipo_js_new_error(ipo_js_vm_t *vm, const char *name, const char *msg) {
	ipo_js_obj_t *o = vm_alloc_obj(vm, JS_OBJ_ERROR, vm->err_proto);
	if (!o) return NULL;
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(o));
	ipo_js_set_prop(vm, o, "name", ipo_js_str_cstr(vm, name ? name : "Error"));
	ipo_js_set_prop(vm, o, "message", ipo_js_str_cstr(vm, msg ? msg : ""));
	ipo_js_gc_pop_roots(vm, top);
	return o;
}

ipo_js_obj_t *ipo_js_new_promise(ipo_js_vm_t *vm) {
	ipo_js_obj_t *o = vm_alloc_obj(vm, JS_OBJ_PROMISE, vm->promise_proto);
	if (o) {
		o->u.prom.state = 0;
		o->u.prom.result = ipo_js_undef();
	}
	return o;
}

static ipo_js_env_t *vm_alloc_env(ipo_js_vm_t *vm, ipo_js_env_t *parent) {
	ipo_js_env_t *e = (ipo_js_env_t *)xcalloc(1, sizeof(ipo_js_env_t));
	if (!e) return NULL;
	e->parent = parent;
	e->this_val = ipo_js_undef();
	e->gc_next = vm->gc_envs;
	vm->gc_envs = e;
	vm->gc_bytes += sizeof(ipo_js_env_t);
	return e;
}

void ipo_js_gc_pin(ipo_js_obj_t *obj) {
	if (obj) obj->pin_count++;
}

void ipo_js_gc_unpin(ipo_js_obj_t *obj) {
	if (obj && obj->pin_count > 0) obj->pin_count--;
}

void ipo_js_gc_mark_str(ipo_js_vm_t *vm, ipo_js_str_t *s) {
	(void)vm;
	if (s) s->marked = 1;
}

static void gc_mark_env(ipo_js_vm_t *vm, ipo_js_env_t *e);

void ipo_js_gc_mark_val(ipo_js_vm_t *vm, ipo_js_val_t v) {
	if (v.type == JS_T_STRING) ipo_js_gc_mark_str(vm, v.u.str);
	else if (v.type == JS_T_OBJECT) ipo_js_gc_mark_obj(vm, v.u.obj);
}

static void gc_scan_obj(ipo_js_vm_t *vm, ipo_js_obj_t *o) {
	ipo_js_gc_mark_obj(vm, o->proto);
	for (size_t i = 0; i < o->prop_len; i++) {
		ipo_js_gc_mark_str(vm, o->props[i].key);
		ipo_js_gc_mark_val(vm, o->props[i].val);
		ipo_js_gc_mark_obj(vm, o->props[i].getter);
		ipo_js_gc_mark_obj(vm, o->props[i].setter);
	}
	if (o->host_mark) o->host_mark(vm, o);
	switch (o->kind) {
	case JS_OBJ_ARRAY:
		for (size_t i = 0; i < o->u.arr.len; i++) {
			ipo_js_gc_mark_val(vm, o->u.arr.items[i]);
		}
		break;
	case JS_OBJ_FUNCTION:
		gc_mark_env(vm, o->u.fn.closure);
		ipo_js_gc_mark_str(vm, o->u.fn.name);
		ipo_js_gc_mark_val(vm, o->u.fn.bound_this);
		for (int i = 0; i < o->u.fn.bound_argc; i++) {
			ipo_js_gc_mark_val(vm, o->u.fn.bound_args[i]);
		}
		break;
	case JS_OBJ_NATIVE_FN:
		ipo_js_gc_mark_str(vm, o->u.nat.name);
		ipo_js_gc_mark_val(vm, o->u.nat.bound_this);
		for (int i = 0; i < o->u.nat.bound_argc; i++) {
			ipo_js_gc_mark_val(vm, o->u.nat.bound_args[i]);
		}
		break;
	case JS_OBJ_REGEXP:
		ipo_js_gc_mark_str(vm, o->u.re.pattern);
		break;
	case JS_OBJ_PROMISE:
		ipo_js_gc_mark_val(vm, o->u.prom.result);
		for (int i = 0; i < o->u.prom.cb_len; i++) {
			ipo_js_gc_mark_val(vm, o->u.prom.then_fns[i]);
			ipo_js_gc_mark_val(vm, o->u.prom.catch_fns[i]);
			ipo_js_gc_mark_val(vm, o->u.prom.then_promises[i]);
		}
		break;
	default:
		break;
	}
}

void ipo_js_gc_mark_obj(ipo_js_vm_t *vm, ipo_js_obj_t *o) {
	if (!o || o->marked) return;
	o->marked = 1;
	if (vm->mark_depth >= 192) {
		if (vm->mark_len >= vm->mark_cap) {
			size_t ncap = vm->mark_cap ? vm->mark_cap * 2 : 128;
			vm->mark_stack = (ipo_js_obj_t **)xrealloc(vm->mark_stack, ncap * sizeof(ipo_js_obj_t *));
			vm->mark_cap = ncap;
		}
		vm->mark_stack[vm->mark_len++] = o;
		return;
	}
	vm->mark_depth++;
	gc_scan_obj(vm, o);
	vm->mark_depth--;
}

static void gc_mark_env(ipo_js_vm_t *vm, ipo_js_env_t *e) {
	while (e && !e->marked) {
		e->marked = 1;
		ipo_js_gc_mark_val(vm, e->this_val);
		ipo_js_gc_mark_obj(vm, e->with_obj);
		for (size_t i = 0; i < e->len; i++) {
			ipo_js_gc_mark_str(vm, e->vars[i].name);
			ipo_js_gc_mark_val(vm, e->vars[i].val);
		}
		e = e->parent;
	}
}

void ipo_js_gc_collect(ipo_js_vm_t *vm) {
	if (!vm) return;
	/* Mark roots */
	ipo_js_gc_mark_obj(vm, vm->global_obj);
	gc_mark_env(vm, vm->global_env);
	ipo_js_gc_mark_obj(vm, vm->obj_proto);
	ipo_js_gc_mark_obj(vm, vm->arr_proto);
	ipo_js_gc_mark_obj(vm, vm->str_proto);
	ipo_js_gc_mark_obj(vm, vm->fn_proto);
	ipo_js_gc_mark_obj(vm, vm->err_proto);
	ipo_js_gc_mark_obj(vm, vm->regexp_proto);
	ipo_js_gc_mark_obj(vm, vm->date_proto);
	ipo_js_gc_mark_obj(vm, vm->promise_proto);
	ipo_js_gc_mark_val(vm, vm->sig_val);
	ipo_js_gc_mark_val(vm, vm->exc_val);

	for (size_t i = 0; i < vm->root_len; i++) {
		ipo_js_gc_mark_val(vm, vm->root_stack[i]);
	}
	for (int i = 0; i < vm->frame_len; i++) {
		gc_mark_env(vm, vm->frames[i].env);
		ipo_js_gc_mark_val(vm, vm->frames[i].this_val);
		ipo_js_gc_mark_obj(vm, vm->frames[i].super_proto);
		ipo_js_gc_mark_obj(vm, vm->frames[i].active_fn);
	}
	for (int i = vm->mt_head; i < vm->mt_len; i++) {
		ipo_js_gc_mark_val(vm, vm->microtasks[i].fn);
		ipo_js_gc_mark_val(vm, vm->microtasks[i].arg);
		ipo_js_gc_mark_obj(vm, vm->microtasks[i].target_prom);
	}
	for (ipo_js_obj_t *o = vm->gc_objs; o; o = o->gc_next) {
		if (o->pin_count > 0) ipo_js_gc_mark_obj(vm, o);
	}
	while (vm->mark_len > 0) {
		ipo_js_obj_t *o = vm->mark_stack[--vm->mark_len];
		vm->mark_depth++;
		gc_scan_obj(vm, o);
		vm->mark_depth--;
	}

	/* Sweep objects */
	size_t bytes = 0;
	ipo_js_obj_t **op = &vm->gc_objs;
	while (*op) {
		ipo_js_obj_t *o = *op;
		if (!o->marked) {
			*op = o->gc_next;
			if (o->host_finalize) o->host_finalize(vm, o);
			free(o->props);
			if (o->kind == JS_OBJ_ARRAY) free(o->u.arr.items);
			else if (o->kind == JS_OBJ_FUNCTION) free(o->u.fn.bound_args);
			else if (o->kind == JS_OBJ_NATIVE_FN) free(o->u.nat.bound_args);
			else if (o->kind == JS_OBJ_PROMISE) {
				free(o->u.prom.then_fns);
				free(o->u.prom.catch_fns);
				free(o->u.prom.then_promises);
			}
			free(o);
		} else {
			o->marked = 0;
			bytes += sizeof(ipo_js_obj_t) + o->prop_cap * sizeof(ipo_js_prop_t);
			if (o->kind == JS_OBJ_ARRAY) bytes += o->u.arr.cap * sizeof(ipo_js_val_t);
			op = &o->gc_next;
		}
	}

	/* Sweep envs */
	ipo_js_env_t **ep = &vm->gc_envs;
	while (*ep) {
		ipo_js_env_t *e = *ep;
		if (!e->marked) {
			*ep = e->gc_next;
			free(e->vars);
			free(e);
		} else {
			e->marked = 0;
			bytes += sizeof(ipo_js_env_t) + e->cap * sizeof(js_binding_t);
			ep = &e->gc_next;
		}
	}

	/* Sweep strings */
	ipo_js_str_t **sp = &vm->gc_strs;
	while (*sp) {
		ipo_js_str_t *s = *sp;
		if (!s->marked) {
			*sp = s->gc_next;
			free(s);
		} else {
			s->marked = 0;
			bytes += sizeof(ipo_js_str_t) + s->len;
			sp = &s->gc_next;
		}
	}

	vm->gc_bytes = bytes;
	vm->gc_threshold = bytes * 2 + 256 * 1024;
}

static void gc_maybe_collect(ipo_js_vm_t *vm) {
	if (vm->gc_bytes > vm->gc_threshold) {
		ipo_js_gc_collect(vm);
	}
}

size_t ipo_js_vm_mem_used(ipo_js_vm_t *vm) {
	return vm ? vm->gc_bytes : 0;
}

/* =========================================================================
 * AST Freeing & Pool Management
 * ========================================================================= */

static void free_ast_node(ipo_js_ast_t *n) {
	if (!n) return;
	switch (n->kind) {
	case AST_BLOCK:
		free(n->u.block.stmts.items);
		break;
	case AST_VAR_DECL:
		free(n->u.var_decl.targets);
		free(n->u.var_decl.inits);
		break;
	case AST_SWITCH:
		for (size_t i = 0; i < n->u.sw.len; i++) free(n->u.sw.cases[i].body.items);
		free(n->u.sw.cases);
		break;
	case AST_BREAK:
	case AST_CONTINUE:
		free(n->u.jump.label);
		break;
	case AST_LABEL:
		free(n->u.labeled.label);
		break;
	case AST_TRY:
		free(n->u.try_s.catch_name);
		break;
	case AST_FUNC:
		free(n->u.fn.name);
		free(n->u.fn.params);
		break;
	case AST_CLASS:
		free(n->u.cls.name);
		for (size_t i = 0; i < n->u.cls.elem_len; i++) free(n->u.cls.elems[i].name);
		free(n->u.cls.elems);
		break;
	case AST_STR:
		free(n->u.s_lit.str);
		break;
	case AST_REGEXP:
		free(n->u.re_lit.pat);
		free(n->u.re_lit.flags);
		break;
	case AST_TEMPLATE:
		for (size_t i = 0; i <= n->u.tmpl.count; i++) free(n->u.tmpl.quasis[i]);
		free(n->u.tmpl.quasis);
		free(n->u.tmpl.qlen);
		free(n->u.tmpl.exprs);
		break;
	case AST_IDENT:
		free(n->u.ident.name);
		break;
	case AST_ARRAY:
		free(n->u.arr.elems.items);
		break;
	case AST_OBJECT:
		for (size_t i = 0; i < n->u.obj.len; i++) free(n->u.obj.props[i].key_name);
		free(n->u.obj.props);
		break;
	case AST_ARR_PAT:
	case AST_OBJ_PAT:
		for (size_t i = 0; i < n->u.pat.len; i++) free(n->u.pat.elems[i].key_name);
		free(n->u.pat.elems);
		break;
	case AST_MEMBER:
		free(n->u.mem.prop_name);
		break;
	case AST_CALL:
	case AST_NEW:
		free(n->u.call.args.items);
		break;
	case AST_SEQ:
		free(n->u.seq.exprs.items);
		break;
	default:
		break;
	}
	free(n);
}

static void free_ast_pool(js_ast_pool_t *p) {
	if (!p) return;
	for (size_t i = 0; i < p->len; i++) free_ast_node(p->nodes[i]);
	free(p->nodes);
	free(p);
}

/* =========================================================================
 * Object Properties & Array Access
 * ========================================================================= */

static ipo_js_prop_t *obj_find_own_prop(ipo_js_obj_t *obj, const char *key, uint32_t hash) {
	if (!obj || !key) return NULL;
	for (size_t i = 0; i < obj->prop_len; i++) {
		ipo_js_str_t *k = obj->props[i].key;
		if (k && k->hash == hash && strcmp(k->data, key) == 0) {
			return &obj->props[i];
		}
	}
	return NULL;
}

static ipo_js_prop_t *obj_ensure_own_prop(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key) {
	uint32_t h = fnv1a_hash(key, strlen(key));
	ipo_js_prop_t *p = obj_find_own_prop(obj, key, h);
	if (p) return p;
	if (obj->prop_len >= obj->prop_cap) {
		size_t ncap = obj->prop_cap ? obj->prop_cap * 2 : 8;
		ipo_js_prop_t *np = (ipo_js_prop_t *)xrealloc(obj->props, ncap * sizeof(ipo_js_prop_t));
		if (!np) return NULL;
		obj->props = np;
		obj->prop_cap = ncap;
	}
	ipo_js_str_t *ks = vm_alloc_str(vm, key, strlen(key));
	p = &obj->props[obj->prop_len++];
	memset(p, 0, sizeof(*p));
	p->key = ks;
	p->val = ipo_js_undef();
	p->flags = 0x03; /* enumerable | writable */
	return p;
}

static int parse_array_index(const char *s, size_t *out_idx) {
	if (!s || !s[0]) return 0;
	if (s[0] == '0' && s[1] == '\0') {
		*out_idx = 0;
		return 1;
	}
	if (s[0] < '1' || s[0] > '9') return 0;
	size_t idx = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9') return 0;
		if (idx > 100000000) return 0;
		idx = idx * 10 + (size_t)(*p - '0');
	}
	*out_idx = idx;
	return 1;
}

int ipo_js_has_own_prop(ipo_js_obj_t *obj, const char *key) {
	if (!obj || !key) return 0;
	if (obj->kind == JS_OBJ_ARRAY) {
		if (strcmp(key, "length") == 0) return 1;
		size_t idx = 0;
		if (parse_array_index(key, &idx) && idx < obj->u.arr.len) return 1;
	}
	uint32_t h = fnv1a_hash(key, strlen(key));
	return obj_find_own_prop(obj, key, h) != NULL;
}

int ipo_js_del_prop(ipo_js_obj_t *obj, const char *key) {
	if (!obj || !key) return 0;
	if (obj->kind == JS_OBJ_ARRAY) {
		size_t idx = 0;
		if (parse_array_index(key, &idx) && idx < obj->u.arr.len) {
			obj->u.arr.items[idx] = ipo_js_undef();
			return 1;
		}
	}
	uint32_t h = fnv1a_hash(key, strlen(key));
	for (size_t i = 0; i < obj->prop_len; i++) {
		ipo_js_str_t *k = obj->props[i].key;
		if (k && k->hash == h && strcmp(k->data, key) == 0) {
			for (size_t j = i + 1; j < obj->prop_len; j++) obj->props[j - 1] = obj->props[j];
			obj->prop_len--;
			return 1;
		}
	}
	return 1;
}

static void arr_resize(ipo_js_vm_t *vm, ipo_js_obj_t *arr, size_t new_len) {
	if (new_len > arr->u.arr.cap) {
		size_t ncap = arr->u.arr.cap ? arr->u.arr.cap : 8;
		while (ncap < new_len) ncap *= 2;
		ipo_js_val_t *ni = (ipo_js_val_t *)xrealloc(arr->u.arr.items, ncap * sizeof(ipo_js_val_t));
		if (!ni) return;
		arr->u.arr.items = ni;
		vm->gc_bytes += (ncap - arr->u.arr.cap) * sizeof(ipo_js_val_t);
		arr->u.arr.cap = ncap;
	}
	for (size_t i = arr->u.arr.len; i < new_len; i++) {
		arr->u.arr.items[i] = ipo_js_undef();
	}
	arr->u.arr.len = new_len;
}

void ipo_js_arr_push(ipo_js_vm_t *vm, ipo_js_obj_t *arr, ipo_js_val_t val) {
	if (!arr || arr->kind != JS_OBJ_ARRAY) return;
	size_t top = ipo_js_gc_push_root(vm, val);
	size_t idx = arr->u.arr.len;
	arr_resize(vm, arr, idx + 1);
	if (idx < arr->u.arr.len) arr->u.arr.items[idx] = val;
	ipo_js_gc_pop_roots(vm, top);
}

int ipo_js_set_prop(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t val) {
	if (!obj || !key) return 0;
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(obj));
	ipo_js_gc_push_root(vm, val);

	if (obj->host_set && obj->host_set(vm, obj, key, val)) {
		ipo_js_gc_pop_roots(vm, top);
		return 1;
	}

	if (obj->kind == JS_OBJ_ARRAY) {
		if (strcmp(key, "length") == 0) {
			double d = ipo_js_to_num(vm, val);
			if (d >= 0 && d < 10000000.0) arr_resize(vm, obj, (size_t)d);
			ipo_js_gc_pop_roots(vm, top);
			return 1;
		}
		size_t idx = 0;
		if (parse_array_index(key, &idx)) {
			if (idx >= obj->u.arr.len) arr_resize(vm, obj, idx + 1);
			if (idx < obj->u.arr.len) obj->u.arr.items[idx] = val;
			ipo_js_gc_pop_roots(vm, top);
			return 1;
		}
	}

	if (obj->kind == JS_OBJ_REGEXP && strcmp(key, "lastIndex") == 0) {
		double d = ipo_js_to_num(vm, val);
		obj->u.re.last_index = d > 0 ? (size_t)d : 0;
		ipo_js_gc_pop_roots(vm, top);
		return 1;
	}

	/* Check prototype chain for setter */
	uint32_t h = fnv1a_hash(key, strlen(key));
	for (ipo_js_obj_t *cur = obj; cur; cur = cur->proto) {
		ipo_js_prop_t *p = obj_find_own_prop(cur, key, h);
		if (p && (p->flags & 0x04) && p->setter) {
			vm_call_obj(vm, ipo_js_obj_val(p->setter), ipo_js_obj_val(obj), 1, &val, cur->proto);
			ipo_js_gc_pop_roots(vm, top);
			return 1;
		}
		if (p && cur == obj) break;
	}

	ipo_js_prop_t *p = obj_ensure_own_prop(vm, obj, key);
	if (p) {
		p->flags &= ~0x04;
		p->getter = NULL;
		p->setter = NULL;
		p->val = val;
	}
	ipo_js_gc_pop_roots(vm, top);
	return 1;
}

static int get_prop_with_receiver(ipo_js_vm_t *vm, ipo_js_obj_t *obj, ipo_js_val_t receiver,
                                  const char *key, ipo_js_val_t *out) {
	if (!obj || !key) {
		*out = ipo_js_undef();
		return 0;
	}
	uint32_t h = fnv1a_hash(key, strlen(key));
	for (ipo_js_obj_t *cur = obj; cur; cur = cur->proto) {
		if (cur->host_get) {
			ipo_js_val_t hv = ipo_js_undef();
			if (cur->host_get(vm, cur, key, &hv)) {
				*out = hv;
				return 1;
			}
		}
		if (cur->kind == JS_OBJ_ARRAY) {
			if (strcmp(key, "length") == 0) {
				*out = ipo_js_num((double)cur->u.arr.len);
				return 1;
			}
			size_t idx = 0;
			if (parse_array_index(key, &idx)) {
				if (idx < cur->u.arr.len) {
					*out = cur->u.arr.items[idx];
					return 1;
				}
			}
		}
		if (cur->kind == JS_OBJ_REGEXP) {
			if (strcmp(key, "source") == 0) {
				*out = ipo_js_str_cstr(vm, cur->u.re.pattern ? cur->u.re.pattern->data : "");
				return 1;
			}
			if (strcmp(key, "lastIndex") == 0) {
				*out = ipo_js_num((double)cur->u.re.last_index);
				return 1;
			}
			if (strcmp(key, "global") == 0) { *out = ipo_js_bool((cur->u.re.flags & 2) != 0); return 1; }
			if (strcmp(key, "ignoreCase") == 0) { *out = ipo_js_bool((cur->u.re.flags & 1) != 0); return 1; }
			if (strcmp(key, "multiline") == 0) { *out = ipo_js_bool((cur->u.re.flags & 4) != 0); return 1; }
			if (strcmp(key, "flags") == 0) {
				char fb[8]; int fi = 0;
				if (cur->u.re.flags & 2) fb[fi++] = 'g';
				if (cur->u.re.flags & 1) fb[fi++] = 'i';
				if (cur->u.re.flags & 4) fb[fi++] = 'm';
				if (cur->u.re.flags & 8) fb[fi++] = 's';
				fb[fi] = '\0';
				*out = ipo_js_str_new(vm, fb, (size_t)fi);
				return 1;
			}
		}
		if (cur->kind == JS_OBJ_FUNCTION || cur->kind == JS_OBJ_NATIVE_FN) {
			if (strcmp(key, "name") == 0 && !obj_find_own_prop(cur, key, h)) {
				ipo_js_str_t *ns = (cur->kind == JS_OBJ_FUNCTION) ? cur->u.fn.name : cur->u.nat.name;
				*out = ipo_js_str_cstr(vm, ns ? ns->data : "");
				return 1;
			}
			if (strcmp(key, "length") == 0 && !obj_find_own_prop(cur, key, h)) {
				size_t n = (cur->kind == JS_OBJ_FUNCTION && cur->u.fn.func_ast)
				               ? cur->u.fn.func_ast->u.fn.param_len : 0;
				*out = ipo_js_num((double)n);
				return 1;
			}
		}
		ipo_js_prop_t *p = obj_find_own_prop(cur, key, h);
		if (p) {
			if (p->flags & 0x04) {
				if (p->getter) {
					*out = vm_call_obj(vm, ipo_js_obj_val(p->getter), receiver, 0, NULL, cur->proto);
				} else {
					*out = ipo_js_undef();
				}
			} else {
				*out = p->val;
			}
			return 1;
		}
	}
	*out = ipo_js_undef();
	return 0;
}

int ipo_js_get_prop(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t *out) {
	return get_prop_with_receiver(vm, obj, ipo_js_obj_val(obj), key, out);
}

static void obj_define_accessor(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key,
                                ipo_js_obj_t *getter, ipo_js_obj_t *setter) {
	ipo_js_prop_t *p = obj_ensure_own_prop(vm, obj, key);
	if (!p) return;
	p->flags |= 0x04;
	if (getter) p->getter = getter;
	if (setter) p->setter = setter;
}

/* =========================================================================
 * Type Conversions & Formatting
 * ========================================================================= */

int ipo_js_to_bool(ipo_js_val_t v) {
	switch (v.type) {
	case JS_T_UNDEFINED:
	case JS_T_NULL:
		return 0;
	case JS_T_BOOL:
		return v.u.b ? 1 : 0;
	case JS_T_NUMBER:
		return !isnan(v.u.num) && v.u.num != 0.0;
	case JS_T_STRING:
		return v.u.str && v.u.str->len > 0;
	case JS_T_OBJECT:
		return v.u.obj != NULL;
	}
	return 0;
}

static double parse_js_num_str(const char *s, size_t len) {
	while (len > 0 && isspace((unsigned char)*s)) { s++; len--; }
	while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
	if (len == 0) return 0.0;
	char *tmp = xstrdup_n(s, len);
	double res = NAN;
	if (strcmp(tmp, "Infinity") == 0 || strcmp(tmp, "+Infinity") == 0) res = INFINITY;
	else if (strcmp(tmp, "-Infinity") == 0) res = -INFINITY;
	else if (len > 2 && tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) {
		char *end = NULL;
		unsigned long long u = strtoull(tmp + 2, &end, 16);
		if (end && *end == '\0') res = (double)u;
	} else if (len > 2 && tmp[0] == '0' && (tmp[1] == 'b' || tmp[1] == 'B')) {
		char *end = NULL;
		unsigned long long u = strtoull(tmp + 2, &end, 2);
		if (end && *end == '\0') res = (double)u;
	} else if (len > 2 && tmp[0] == '0' && (tmp[1] == 'o' || tmp[1] == 'O')) {
		char *end = NULL;
		unsigned long long u = strtoull(tmp + 2, &end, 8);
		if (end && *end == '\0') res = (double)u;
	} else {
		char *end = NULL;
		double d = strtod(tmp, &end);
		if (end == tmp + len) res = d;
	}
	free(tmp);
	return res;
}

static ipo_js_val_t to_primitive(ipo_js_vm_t *vm, ipo_js_val_t v, int hint_str) {
	if (v.type != JS_T_OBJECT || !v.u.obj) return v;
	const char *m1 = hint_str ? "toString" : "valueOf";
	const char *m2 = hint_str ? "valueOf" : "toString";
	ipo_js_val_t fn = ipo_js_undef();
	if (ipo_js_get_prop(vm, v.u.obj, m1, &fn) && ipo_js_is_callable(fn)) {
		ipo_js_val_t r = vm_call_obj(vm, fn, v, 0, NULL, NULL);
		if (vm->sig != SIG_NONE || r.type != JS_T_OBJECT) return r;
	}
	if (ipo_js_get_prop(vm, v.u.obj, m2, &fn) && ipo_js_is_callable(fn)) {
		ipo_js_val_t r = vm_call_obj(vm, fn, v, 0, NULL, NULL);
		if (vm->sig != SIG_NONE || r.type != JS_T_OBJECT) return r;
	}
	return ipo_js_str_cstr(vm, "[object Object]");
}

double ipo_js_to_num(ipo_js_vm_t *vm, ipo_js_val_t v) {
	if (v.type == JS_T_OBJECT && v.u.obj) v = to_primitive(vm, v, 0);
	switch (v.type) {
	case JS_T_UNDEFINED: return NAN;
	case JS_T_NULL:      return 0.0;
	case JS_T_BOOL:      return v.u.b ? 1.0 : 0.0;
	case JS_T_NUMBER:    return v.u.num;
	case JS_T_STRING:    return v.u.str ? parse_js_num_str(v.u.str->data, v.u.str->len) : 0.0;
	case JS_T_OBJECT:    return NAN;
	}
	return NAN;
}

int32_t ipo_js_to_i32(ipo_js_vm_t *vm, ipo_js_val_t v) {
	double d = ipo_js_to_num(vm, v);
	if (!isfinite(d) || d == 0.0) return 0;
	double t = trunc(d);
	int64_t i = (int64_t)fmod(t, 4294967296.0);
	return (int32_t)(uint32_t)i;
}

static size_t fmt_js_number(double num, char *buf, size_t cap) {
	if (isnan(num)) {
		snprintf(buf, cap, "NaN");
		return strlen(buf);
	}
	if (isinf(num)) {
		snprintf(buf, cap, "%s", num > 0 ? "Infinity" : "-Infinity");
		return strlen(buf);
	}
	if (num == 0.0) {
		snprintf(buf, cap, "0");
		return 1;
	}
	double a = fabs(num);
	if (a <= 9007199254740991.0) {
		double t = trunc(num);
		if (t == num) {
			snprintf(buf, cap, "%lld", (long long)t);
			return strlen(buf);
		}
	}
	if (a >= 1e21 || a < 1e-6) {
		int exp10 = (int)floor(log10(a));
		double mant = num / pow(10.0, (double)exp10);
		if (fabs(mant) >= 10.0) { mant /= 10.0; exp10++; }
		else if (fabs(mant) < 1.0 && mant != 0.0) { mant *= 10.0; exp10--; }
		char mb[48];
		snprintf(mb, sizeof(mb), "%.12f", mant);
		char *dot = strchr(mb, '.');
		if (dot) {
			char *end = mb + strlen(mb) - 1;
			while (end > dot && *end == '0') *end-- = '\0';
			if (end == dot) *end = '\0';
		}
		snprintf(buf, cap, "%se%s%d", mb, exp10 >= 0 ? "+" : "", exp10);
		return strlen(buf);
	}
	snprintf(buf, cap, "%.12f", num);
	char *dot = strchr(buf, '.');
	if (dot) {
		char *end = buf + strlen(buf) - 1;
		while (end > dot && *end == '0') *end-- = '\0';
		if (end == dot) *end = '\0';
	}
	if (strcmp(buf, "-0") == 0) strcpy(buf, "0");
	return strlen(buf);
}

ipo_js_str_t *ipo_js_to_str(ipo_js_vm_t *vm, ipo_js_val_t v) {
	if (v.type == JS_T_OBJECT && v.u.obj) v = to_primitive(vm, v, 1);
	switch (v.type) {
	case JS_T_STRING:    return v.u.str;
	case JS_T_UNDEFINED: return vm_alloc_str(vm, "undefined", 9);
	case JS_T_NULL:      return vm_alloc_str(vm, "null", 4);
	case JS_T_BOOL:      return v.u.b ? vm_alloc_str(vm, "true", 4) : vm_alloc_str(vm, "false", 5);
	case JS_T_NUMBER: {
		char buf[64];
		size_t n = fmt_js_number(v.u.num, buf, sizeof(buf));
		return vm_alloc_str(vm, buf, n);
	}
	case JS_T_OBJECT:
		return vm_alloc_str(vm, "[object Object]", 15);
	}
	return vm_alloc_str(vm, "", 0);
}

const char *ipo_js_cstr(ipo_js_vm_t *vm, ipo_js_val_t v, size_t *out_len) {
	ipo_js_str_t *s = ipo_js_to_str(vm, v);
	if (out_len) *out_len = s ? s->len : 0;
	return s ? s->data : "";
}

int ipo_js_is_callable(ipo_js_val_t v) {
	if (v.type != JS_T_OBJECT || !v.u.obj) return 0;
	return v.u.obj->kind == JS_OBJ_FUNCTION || v.u.obj->kind == JS_OBJ_NATIVE_FN;
}

static int vm_get_prop_val(ipo_js_vm_t *vm, ipo_js_val_t obj_v, const char *key, ipo_js_val_t *out) {
	if (obj_v.type == JS_T_STRING && obj_v.u.str) {
		if (strcmp(key, "length") == 0) {
			*out = ipo_js_num((double)obj_v.u.str->len);
			return 1;
		}
		size_t idx = 0;
		if (parse_array_index(key, &idx) && idx < obj_v.u.str->len) {
			*out = ipo_js_str_new(vm, &obj_v.u.str->data[idx], 1);
			return 1;
		}
		return get_prop_with_receiver(vm, vm->str_proto, obj_v, key, out);
	}
	if (obj_v.type == JS_T_NUMBER || obj_v.type == JS_T_BOOL) {
		return get_prop_with_receiver(vm, vm->obj_proto, obj_v, key, out);
	}
	if (obj_v.type != JS_T_OBJECT || !obj_v.u.obj) {
		ipo_js_throw_msg(vm, "TypeError", "Cannot read properties of %s (reading '%s')",
		                 obj_v.type == JS_T_NULL ? "null" : "undefined", key);
		*out = ipo_js_undef();
		return 0;
	}
	return get_prop_with_receiver(vm, obj_v.u.obj, obj_v, key, out);
}

static int vm_set_prop_val(ipo_js_vm_t *vm, ipo_js_val_t obj_v, const char *key, ipo_js_val_t val) {
	if (obj_v.type != JS_T_OBJECT || !obj_v.u.obj) {
		if (obj_v.type == JS_T_UNDEFINED || obj_v.type == JS_T_NULL) {
			ipo_js_throw_msg(vm, "TypeError", "Cannot set properties of %s (setting '%s')",
			                 obj_v.type == JS_T_NULL ? "null" : "undefined", key);
		}
		return 0;
	}
	return ipo_js_set_prop(vm, obj_v.u.obj, key, val);
}

/* =========================================================================
 * Exceptions
 * ========================================================================= */

void ipo_js_throw(ipo_js_vm_t *vm, ipo_js_val_t err) {
	if (!vm) return;
	vm->has_exc = 1;
	vm->exc_val = err;
	vm->sig = SIG_THROW;
	vm->sig_val = err;
}

void ipo_js_throw_msg(ipo_js_vm_t *vm, const char *err_type, const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ipo_js_obj_t *e = ipo_js_new_error(vm, err_type ? err_type : "Error", buf);
	ipo_js_throw(vm, ipo_js_obj_val(e));
}

int ipo_js_has_exception(ipo_js_vm_t *vm) {
	return vm && vm->has_exc;
}

ipo_js_val_t ipo_js_get_exception(ipo_js_vm_t *vm) {
	return vm ? vm->exc_val : ipo_js_undef();
}

void ipo_js_clear_exception(ipo_js_vm_t *vm) {
	if (!vm) return;
	vm->has_exc = 0;
	vm->exc_val = ipo_js_undef();
	if (vm->sig == SIG_THROW) {
		vm->sig = SIG_NONE;
		vm->sig_val = ipo_js_undef();
	}
}

/* =========================================================================
 * Equality Helpers
 * ========================================================================= */

static int js_strict_eq(ipo_js_val_t a, ipo_js_val_t b) {
	if (a.type != b.type) return 0;
	switch (a.type) {
	case JS_T_UNDEFINED:
	case JS_T_NULL:
		return 1;
	case JS_T_BOOL:
		return a.u.b == b.u.b;
	case JS_T_NUMBER:
		if (isnan(a.u.num) || isnan(b.u.num)) return 0;
		return a.u.num == b.u.num;
	case JS_T_STRING:
		return a.u.str->len == b.u.str->len &&
		       a.u.str->hash == b.u.str->hash &&
		       memcmp(a.u.str->data, b.u.str->data, a.u.str->len) == 0;
	case JS_T_OBJECT:
		return a.u.obj == b.u.obj;
	}
	return 0;
}

static int js_loose_eq(ipo_js_vm_t *vm, ipo_js_val_t a, ipo_js_val_t b) {
	if (a.type == b.type) return js_strict_eq(a, b);
	if ((a.type == JS_T_NULL && b.type == JS_T_UNDEFINED) ||
	    (a.type == JS_T_UNDEFINED && b.type == JS_T_NULL)) return 1;
	if (a.type == JS_T_BOOL) return js_loose_eq(vm, ipo_js_num(a.u.b ? 1.0 : 0.0), b);
	if (b.type == JS_T_BOOL) return js_loose_eq(vm, a, ipo_js_num(b.u.b ? 1.0 : 0.0));
	if (a.type == JS_T_NUMBER && b.type == JS_T_STRING)
		return a.u.num == ipo_js_to_num(vm, b);
	if (a.type == JS_T_STRING && b.type == JS_T_NUMBER)
		return ipo_js_to_num(vm, a) == b.u.num;
	if (a.type == JS_T_OBJECT && (b.type == JS_T_STRING || b.type == JS_T_NUMBER))
		return js_loose_eq(vm, to_primitive(vm, a, 0), b);
	if (b.type == JS_T_OBJECT && (a.type == JS_T_STRING || a.type == JS_T_NUMBER))
		return js_loose_eq(vm, a, to_primitive(vm, b, 0));
	return 0;
}

/* =========================================================================
 * Built-in RegExp Engine (NFA backtracking with ReDoS step limit)
 * ========================================================================= */

typedef enum {
	RE_LIT = 0,
	RE_ANY,
	RE_BOL,
	RE_EOL,
	RE_BOUNDARY,
	RE_NONBOUND,
	RE_CHARCLASS,
	RE_GROUP_START,
	RE_GROUP_END,
	RE_LOOKAHEAD,
	RE_NEG_LOOKAHEAD,
	RE_ALT,
	RE_REPEAT,
	RE_BACKREF
} re_node_kind_t;

typedef struct re_node re_node_t;
typedef struct re_seq {
	re_node_t **nodes;
	size_t len;
	size_t cap;
} re_seq_t;

struct re_node {
	re_node_kind_t kind;
	uint8_t ch;
	uint8_t bitmap[32];
	int negated;
	int group_idx;
	int min_rep;
	int max_rep; /* -1 = unbounded */
	int lazy;
	re_node_t *child;
	re_seq_t *branches;
	size_t branch_len;
	size_t branch_cap;
};

typedef struct re_prog {
	re_node_t *root;
	int num_groups;
	uint8_t flags;
	const char *err;
	re_node_t **allocs;
	size_t alloc_len;
	size_t alloc_cap;
} re_prog_t;

typedef struct re_compiler {
	const char *p;
	const char *end;
	re_prog_t *prog;
} re_compiler_t;

static re_node_t *re_new_node(re_compiler_t *c, re_node_kind_t k) {
	re_node_t *n = (re_node_t *)xcalloc(1, sizeof(re_node_t));
	n->kind = k;
	if (c->prog->alloc_len >= c->prog->alloc_cap) {
		size_t ncap = c->prog->alloc_cap ? c->prog->alloc_cap * 2 : 32;
		c->prog->allocs = (re_node_t **)xrealloc(c->prog->allocs, ncap * sizeof(re_node_t *));
		c->prog->alloc_cap = ncap;
	}
	c->prog->allocs[c->prog->alloc_len++] = n;
	return n;
}

static void re_bm_set(uint8_t bm[32], uint8_t ch) { bm[ch >> 3] |= (uint8_t)(1u << (ch & 7)); }
static int  re_bm_get(const uint8_t bm[32], uint8_t ch) { return (bm[ch >> 3] >> (ch & 7)) & 1; }

static void re_bm_add_shorthand(uint8_t bm[32], char code) {
	for (int i = 0; i < 256; i++) {
		int m = 0;
		switch (code) {
		case 'd': m = (i >= '0' && i <= '9'); break;
		case 'D': m = !(i >= '0' && i <= '9'); break;
		case 'w': m = (isalnum(i) || i == '_'); break;
		case 'W': m = !(isalnum(i) || i == '_'); break;
		case 's': m = (i == ' ' || i == '\t' || i == '\n' || i == '\r' || i == '\f' || i == '\v'); break;
		case 'S': m = !(i == ' ' || i == '\t' || i == '\n' || i == '\r' || i == '\f' || i == '\v'); break;
		}
		if (m) re_bm_set(bm, (uint8_t)i);
	}
}

static re_node_t *re_parse_alt(re_compiler_t *c);

static int re_hex_digit(char ch) {
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

static uint8_t re_parse_esc_char(re_compiler_t *c) {
	if (c->p >= c->end) return '\\';
	char ch = *c->p++;
	switch (ch) {
	case 'n': return '\n';
	case 'r': return '\r';
	case 't': return '\t';
	case 'f': return '\f';
	case 'v': return '\v';
	case '0': return '\0';
	case 'x':
		if (c->p + 1 < c->end) {
			int a = re_hex_digit(c->p[0]), b = re_hex_digit(c->p[1]);
			if (a >= 0 && b >= 0) { c->p += 2; return (uint8_t)((a << 4) | b); }
		}
		return 'x';
	default: return (uint8_t)ch;
	}
}

static re_node_t *re_parse_class(re_compiler_t *c) {
	re_node_t *n = re_new_node(c, RE_CHARCLASS);
	if (c->p < c->end && *c->p == '^') { n->negated = 1; c->p++; }
	int first = 1;
	int have_prev = 0;
	uint8_t prev = 0;
	while (c->p < c->end) {
		char ch = *c->p++;
		if (ch == ']' && !first) return n;
		first = 0;
		if (ch == '\\' && c->p < c->end) {
			char nxt = *c->p;
			if (nxt == 'd' || nxt == 'D' || nxt == 'w' || nxt == 'W' || nxt == 's' || nxt == 'S') {
				c->p++;
				re_bm_add_shorthand(n->bitmap, nxt);
				have_prev = 0;
				continue;
			}
			uint8_t ev = re_parse_esc_char(c);
			re_bm_set(n->bitmap, ev);
			prev = ev;
			have_prev = 1;
		} else if (ch == '-' && have_prev && c->p < c->end && *c->p != ']') {
			uint8_t hi = (uint8_t)*c->p++;
			if (hi == '\\' && c->p < c->end) hi = re_parse_esc_char(c);
			for (int k = prev; k <= hi; k++) re_bm_set(n->bitmap, (uint8_t)k);
			have_prev = 0;
		} else {
			re_bm_set(n->bitmap, (uint8_t)ch);
			prev = (uint8_t)ch;
			have_prev = 1;
		}
	}
	c->prog->err = "Unterminated character class";
	return n;
}

static re_node_t *re_wrap_seq(re_compiler_t *c, re_seq_t seq) {
	re_node_t *n = re_new_node(c, RE_ALT);
	n->branches = (re_seq_t *)xmalloc(sizeof(re_seq_t));
	n->branches[0] = seq;
	n->branch_len = 1;
	n->branch_cap = 1;
	return n;
}

static re_node_t *re_parse_atom(re_compiler_t *c) {
	if (c->p >= c->end) return NULL;
	char ch = *c->p;
	if (ch == '^') { c->p++; return re_new_node(c, RE_BOL); }
	if (ch == '$') { c->p++; return re_new_node(c, RE_EOL); }
	if (ch == '.') { c->p++; return re_new_node(c, RE_ANY); }
	if (ch == '[') { c->p++; return re_parse_class(c); }
	if (ch == '(') {
		c->p++;
		int kind = 0; /* 0=cap, 1=noncap, 2=lookahead, 3=neg_lookahead */
		if (c->p + 1 < c->end && c->p[0] == '?') {
			if (c->p[1] == ':') { kind = 1; c->p += 2; }
			else if (c->p[1] == '=') { kind = 2; c->p += 2; }
			else if (c->p[1] == '!') { kind = 3; c->p += 2; }
		}
		int gidx = -1;
		if (kind == 0) gidx = ++c->prog->num_groups;
		re_node_t *inner = re_parse_alt(c);
		if (c->p < c->end && *c->p == ')') c->p++;
		else c->prog->err = "Unterminated group";
		if (kind == 2 || kind == 3) {
			re_node_t *lk = re_new_node(c, kind == 2 ? RE_LOOKAHEAD : RE_NEG_LOOKAHEAD);
			lk->child = inner;
			return lk;
		}
		if (kind == 1) return inner;
		re_seq_t seq = { NULL, 0, 0 };
		seq.nodes = (re_node_t **)xmalloc(3 * sizeof(re_node_t *));
		re_node_t *gs = re_new_node(c, RE_GROUP_START); gs->group_idx = gidx;
		re_node_t *ge = re_new_node(c, RE_GROUP_END);   ge->group_idx = gidx;
		seq.nodes[0] = gs;
		seq.nodes[1] = inner;
		seq.nodes[2] = ge;
		seq.len = 3;
		seq.cap = 3;
		return re_wrap_seq(c, seq);
	}
	if (ch == '\\') {
		c->p++;
		if (c->p >= c->end) {
			re_node_t *n = re_new_node(c, RE_LIT); n->ch = '\\'; return n;
		}
		char nxt = *c->p;
		if (nxt == 'b') { c->p++; return re_new_node(c, RE_BOUNDARY); }
		if (nxt == 'B') { c->p++; return re_new_node(c, RE_NONBOUND); }
		if (nxt == 'd' || nxt == 'D' || nxt == 'w' || nxt == 'W' || nxt == 's' || nxt == 'S') {
			c->p++;
			re_node_t *n = re_new_node(c, RE_CHARCLASS);
			re_bm_add_shorthand(n->bitmap, nxt);
			return n;
		}
		if (nxt >= '1' && nxt <= '9') {
			c->p++;
			re_node_t *n = re_new_node(c, RE_BACKREF);
			n->group_idx = nxt - '0';
			return n;
		}
		re_node_t *n = re_new_node(c, RE_LIT);
		n->ch = re_parse_esc_char(c);
		return n;
	}
	c->p++;
	re_node_t *n = re_new_node(c, RE_LIT);
	n->ch = (uint8_t)ch;
	return n;
}

static re_node_t *re_parse_piece(re_compiler_t *c) {
	re_node_t *atom = re_parse_atom(c);
	if (!atom || c->p >= c->end) return atom;
	int min_r = 1, max_r = 1, is_rep = 0;
	char ch = *c->p;
	if (ch == '*') { min_r = 0; max_r = -1; is_rep = 1; c->p++; }
	else if (ch == '+') { min_r = 1; max_r = -1; is_rep = 1; c->p++; }
	else if (ch == '?') { min_r = 0; max_r = 1; is_rep = 1; c->p++; }
	else if (ch == '{') {
		const char *save = c->p++;
		int a = 0, has_a = 0, b = -1, has_comma = 0;
		while (c->p < c->end && isdigit((unsigned char)*c->p)) {
			has_a = 1; a = a * 10 + (*c->p++ - '0');
		}
		if (has_a && c->p < c->end && *c->p == ',') {
			has_comma = 1; c->p++;
			int bval = 0, has_b = 0;
			while (c->p < c->end && isdigit((unsigned char)*c->p)) {
				has_b = 1; bval = bval * 10 + (*c->p++ - '0');
			}
			b = has_b ? bval : -1;
		} else if (has_a) {
			b = a;
		}
		if (has_a && c->p < c->end && *c->p == '}') {
			c->p++;
			min_r = a;
			max_r = has_comma ? b : a;
			is_rep = 1;
		} else {
			c->p = save;
		}
	}
	if (!is_rep) return atom;
	re_node_t *r = re_new_node(c, RE_REPEAT);
	r->child = atom;
	r->min_rep = min_r;
	r->max_rep = max_r;
	if (c->p < c->end && *c->p == '?') { r->lazy = 1; c->p++; }
	return r;
}

static re_seq_t re_parse_branch(re_compiler_t *c) {
	re_seq_t seq = { NULL, 0, 0 };
	while (c->p < c->end && *c->p != '|' && *c->p != ')') {
		re_node_t *n = re_parse_piece(c);
		if (!n) break;
		if (seq.len >= seq.cap) {
			size_t ncap = seq.cap ? seq.cap * 2 : 8;
			seq.nodes = (re_node_t **)xrealloc(seq.nodes, ncap * sizeof(re_node_t *));
			seq.cap = ncap;
		}
		seq.nodes[seq.len++] = n;
	}
	return seq;
}

static re_node_t *re_parse_alt(re_compiler_t *c) {
	re_node_t *alt = re_new_node(c, RE_ALT);
	for (;;) {
		re_seq_t br = re_parse_branch(c);
		if (alt->branch_len >= alt->branch_cap) {
			size_t ncap = alt->branch_cap ? alt->branch_cap * 2 : 4;
			alt->branches = (re_seq_t *)xrealloc(alt->branches, ncap * sizeof(re_seq_t));
			alt->branch_cap = ncap;
		}
		alt->branches[alt->branch_len++] = br;
		if (c->p < c->end && *c->p == '|') c->p++;
		else break;
	}
	return alt;
}

static re_prog_t *re_compile(const char *pat, size_t plen, uint8_t flags) {
	re_prog_t *prog = (re_prog_t *)xcalloc(1, sizeof(re_prog_t));
	prog->flags = flags;
	re_compiler_t comp;
	comp.p = pat ? pat : "";
	comp.end = comp.p + plen;
	comp.prog = prog;
	prog->root = re_parse_alt(&comp);
	return prog;
}

static void re_free(re_prog_t *prog) {
	if (!prog) return;
	for (size_t i = 0; i < prog->alloc_len; i++) {
		re_node_t *n = prog->allocs[i];
		if (n->kind == RE_ALT) {
			for (size_t j = 0; j < n->branch_len; j++) free(n->branches[j].nodes);
			free(n->branches);
		}
		free(n);
	}
	free(prog->allocs);
	free(prog);
}

#define RE_MAX_GROUPS 32
typedef struct re_state {
	const char *s;
	size_t slen;
	uint8_t flags;
	int cap_start[RE_MAX_GROUPS];
	int cap_end[RE_MAX_GROUPS];
	int steps;
	int depth;
} re_state_t;

typedef struct re_cont {
	const re_seq_t *seq;
	size_t idx;
	const struct re_cont *next;
} re_cont_t;

static int re_is_word(uint8_t c) { return isalnum(c) || c == '_'; }

static int re_match_node(re_state_t *st, const re_node_t *n, size_t pos, const re_cont_t *cont, size_t *out_end);

static int re_run_cont(re_state_t *st, size_t pos, const re_cont_t *cont, size_t *out_end) {
	while (cont && cont->idx >= cont->seq->len) cont = cont->next;
	if (!cont) {
		*out_end = pos;
		return 1;
	}
	re_cont_t nxt = { cont->seq, cont->idx + 1, cont->next };
	return re_match_node(st, cont->seq->nodes[cont->idx], pos, &nxt, out_end);
}

static int re_match_repeat_inner(re_state_t *st, const re_node_t *rep, int count, size_t pos,
                                 size_t last_zero_pos, const re_cont_t *cont, size_t *out_end);

static int re_match_repeat(re_state_t *st, const re_node_t *rep, int count, size_t pos,
                           size_t last_zero_pos, const re_cont_t *cont, size_t *out_end) {
	if (st->depth >= 256) return 0;
	st->depth++;
	int r = re_match_repeat_inner(st, rep, count, pos, last_zero_pos, cont, out_end);
	st->depth--;
	return r;
}

static int re_match_repeat_inner(re_state_t *st, const re_node_t *rep, int count, size_t pos,
                                 size_t last_zero_pos, const re_cont_t *cont, size_t *out_end) {
	if (++st->steps > 200000) return 0;
	if (rep->max_rep >= 0 && count >= rep->max_rep) {
		return re_run_cont(st, pos, cont, out_end);
	}
	if (count >= rep->min_rep && rep->lazy) {
		if (re_run_cont(st, pos, cont, out_end)) return 1;
	}
	/* Try matching one more iteration, then continuing this repeat */
	/* We represent matching rep->child then continuing repeat via callback recursion using temporary cont */
	int icase = (st->flags & 1) != 0;
	const re_node_t *c = rep->child;
	/* Simple fast-path for single-character atoms avoids deep recursion on huge strings */
	if (c->kind == RE_LIT || c->kind == RE_ANY || c->kind == RE_CHARCLASS) {
		size_t max_pos = pos;
		int cur_cnt = count;
		while ((rep->max_rep < 0 || cur_cnt < rep->max_rep) && max_pos < st->slen) {
			uint8_t ch = (uint8_t)st->s[max_pos];
			int ok = 0;
			if (c->kind == RE_LIT) {
				ok = icase ? (tolower(ch) == tolower(c->ch)) : (ch == c->ch);
			} else if (c->kind == RE_ANY) {
				ok = (st->flags & 8) ? 1 : (ch != '\n' && ch != '\r');
			} else {
				ok = re_bm_get(c->bitmap, ch);
				if (!ok && icase) {
					if (islower(ch)) ok = re_bm_get(c->bitmap, (uint8_t)toupper(ch));
					else if (isupper(ch)) ok = re_bm_get(c->bitmap, (uint8_t)tolower(ch));
				}
				if (c->negated) ok = !ok;
			}
			if (!ok) break;
			max_pos++;
			cur_cnt++;
			if (rep->lazy && cur_cnt >= rep->min_rep) {
				if (re_run_cont(st, max_pos, cont, out_end)) return 1;
			}
		}
		if (rep->lazy) return 0;
		if (cur_cnt < rep->min_rep) return 0;
		for (size_t try_pos = max_pos; ; try_pos--) {
			if (re_run_cont(st, try_pos, cont, out_end)) return 1;
			if (try_pos == pos + (size_t)(rep->min_rep - count)) break;
		}
		return 0;
	}

	/* General repeat for groups/alternations */
	size_t step_end = 0;
	int saved_s[RE_MAX_GROUPS], saved_e[RE_MAX_GROUPS];
	memcpy(saved_s, st->cap_start, sizeof(saved_s));
	memcpy(saved_e, st->cap_end, sizeof(saved_e));

	if (re_match_node(st, c, pos, NULL, &step_end)) {
		if (step_end > pos || count < rep->min_rep) {
			if (re_match_repeat(st, rep, count + 1, step_end, step_end, cont, out_end)) return 1;
		}
	}
	memcpy(st->cap_start, saved_s, sizeof(saved_s));
	memcpy(st->cap_end, saved_e, sizeof(saved_e));
	if (count >= rep->min_rep && !rep->lazy) {
		return re_run_cont(st, pos, cont, out_end);
	}
	(void)last_zero_pos;
	return 0;
}

static int re_match_node_inner(re_state_t *st, const re_node_t *n, size_t pos,
                               const re_cont_t *cont, size_t *out_end);

static int re_match_node(re_state_t *st, const re_node_t *n, size_t pos,
                         const re_cont_t *cont, size_t *out_end) {
	if (st->depth >= 256) return 0;
	st->depth++;
	int r = re_match_node_inner(st, n, pos, cont, out_end);
	st->depth--;
	return r;
}

static int re_match_node_inner(re_state_t *st, const re_node_t *n, size_t pos,
                               const re_cont_t *cont, size_t *out_end) {
	if (++st->steps > 200000) return 0;
	int icase = (st->flags & 1) != 0;
	int mline = (st->flags & 4) != 0;
	switch (n->kind) {
	case RE_LIT:
		if (pos >= st->slen) return 0;
		if (icase ? (tolower((uint8_t)st->s[pos]) != tolower(n->ch))
		          : ((uint8_t)st->s[pos] != n->ch)) return 0;
		return re_run_cont(st, pos + 1, cont, out_end);
	case RE_ANY:
		if (pos >= st->slen) return 0;
		if (!(st->flags & 8) && (st->s[pos] == '\n' || st->s[pos] == '\r')) return 0;
		return re_run_cont(st, pos + 1, cont, out_end);
	case RE_CHARCLASS: {
		if (pos >= st->slen) return 0;
		uint8_t ch = (uint8_t)st->s[pos];
		int ok = re_bm_get(n->bitmap, ch);
		if (!ok && icase) {
			if (islower(ch)) ok = re_bm_get(n->bitmap, (uint8_t)toupper(ch));
			else if (isupper(ch)) ok = re_bm_get(n->bitmap, (uint8_t)tolower(ch));
		}
		if (n->negated) ok = !ok;
		if (!ok) return 0;
		return re_run_cont(st, pos + 1, cont, out_end);
	}
	case RE_BOL:
		if (pos == 0 || (mline && st->s[pos - 1] == '\n'))
			return re_run_cont(st, pos, cont, out_end);
		return 0;
	case RE_EOL:
		if (pos == st->slen || (mline && st->s[pos] == '\n'))
			return re_run_cont(st, pos, cont, out_end);
		return 0;
	case RE_BOUNDARY:
	case RE_NONBOUND: {
		int w_prev = (pos > 0) ? re_is_word((uint8_t)st->s[pos - 1]) : 0;
		int w_cur  = (pos < st->slen) ? re_is_word((uint8_t)st->s[pos]) : 0;
		int is_b = (w_prev != w_cur);
		if (is_b != (n->kind == RE_BOUNDARY)) return 0;
		return re_run_cont(st, pos, cont, out_end);
	}
	case RE_GROUP_START: {
		int old = -1;
		if (n->group_idx < RE_MAX_GROUPS) {
			old = st->cap_start[n->group_idx];
			st->cap_start[n->group_idx] = (int)pos;
		}
		if (re_run_cont(st, pos, cont, out_end)) return 1;
		if (n->group_idx < RE_MAX_GROUPS) st->cap_start[n->group_idx] = old;
		return 0;
	}
	case RE_GROUP_END: {
		int old = -1;
		if (n->group_idx < RE_MAX_GROUPS) {
			old = st->cap_end[n->group_idx];
			st->cap_end[n->group_idx] = (int)pos;
		}
		if (re_run_cont(st, pos, cont, out_end)) return 1;
		if (n->group_idx < RE_MAX_GROUPS) st->cap_end[n->group_idx] = old;
		return 0;
	}
	case RE_BACKREF: {
		int gi = n->group_idx;
		if (gi >= RE_MAX_GROUPS || st->cap_start[gi] < 0 || st->cap_end[gi] < 0)
			return re_run_cont(st, pos, cont, out_end);
		int glen = st->cap_end[gi] - st->cap_start[gi];
		if (pos + (size_t)glen > st->slen) return 0;
		for (int i = 0; i < glen; i++) {
			uint8_t a = (uint8_t)st->s[st->cap_start[gi] + i];
			uint8_t b = (uint8_t)st->s[pos + (size_t)i];
			if (icase ? (tolower(a) != tolower(b)) : (a != b)) return 0;
		}
		return re_run_cont(st, pos + (size_t)glen, cont, out_end);
	}
	case RE_LOOKAHEAD:
	case RE_NEG_LOOKAHEAD: {
		size_t dummy = 0;
		int m = re_match_node(st, n->child, pos, NULL, &dummy);
		if (n->kind == RE_LOOKAHEAD ? !m : m) return 0;
		return re_run_cont(st, pos, cont, out_end);
	}
	case RE_ALT: {
		int saved_s[RE_MAX_GROUPS], saved_e[RE_MAX_GROUPS];
		memcpy(saved_s, st->cap_start, sizeof(saved_s));
		memcpy(saved_e, st->cap_end, sizeof(saved_e));
		for (size_t i = 0; i < n->branch_len; i++) {
			re_cont_t sub = { &n->branches[i], 0, cont };
			if (re_run_cont(st, pos, &sub, out_end)) return 1;
			memcpy(st->cap_start, saved_s, sizeof(saved_s));
			memcpy(st->cap_end, saved_e, sizeof(saved_e));
		}
		return 0;
	}
	case RE_REPEAT:
		return re_match_repeat(st, n, 0, pos, pos, cont, out_end);
	}
	return 0;
}

static int re_exec_at(re_prog_t *prog, const char *s, size_t slen, size_t start_pos,
                      re_state_t *st_out, size_t *match_start, size_t *match_end) {
	re_state_t st;
	st.s = s;
	st.slen = slen;
	st.flags = prog->flags;
	st.steps = 0;
	for (size_t p = start_pos; p <= slen; p++) {
		for (int i = 0; i < RE_MAX_GROUPS; i++) { st.cap_start[i] = -1; st.cap_end[i] = -1; }
		size_t end_p = 0;
		if (re_match_node(&st, prog->root, p, NULL, &end_p)) {
			*match_start = p;
			*match_end = end_p;
			if (st_out) *st_out = st;
			return 1;
		}
	}
	return 0;
}

static uint8_t parse_re_flags(const char *fs) {
	uint8_t fl = 0;
	if (!fs) return 0;
	for (; *fs; fs++) {
		if (*fs == 'i') fl |= 1;
		else if (*fs == 'g') fl |= 2;
		else if (*fs == 'm') fl |= 4;
		else if (*fs == 's') fl |= 8;
	}
	return fl;
}

static ipo_js_obj_t *vm_new_regexp(ipo_js_vm_t *vm, const char *pat, size_t plen, uint8_t flags) {
	ipo_js_obj_t *o = vm_alloc_obj(vm, JS_OBJ_REGEXP, vm->regexp_proto);
	if (!o) return NULL;
	o->u.re.pattern = vm_alloc_str(vm, pat ? pat : "", plen);
	o->u.re.flags = flags;
	o->u.re.last_index = 0;
	return o;
}

/* =========================================================================
 * Lexer & Recursive-Descent Parser
 * ========================================================================= */

typedef enum {
	TOK_EOF = 0,
	TOK_NUM,
	TOK_STR,
	TOK_REGEX,
	TOK_TMPL_FULL,   /* `plain` */
	TOK_TMPL_HEAD,   /* `head${ */
	TOK_TMPL_MID,    /* }mid${ */
	TOK_TMPL_TAIL,   /* }tail` */
	TOK_IDENT,

	/* Keywords */
	TOK_VAR, TOK_LET, TOK_CONST, TOK_FUNCTION, TOK_RETURN,
	TOK_IF, TOK_ELSE, TOK_WHILE, TOK_DO, TOK_FOR, TOK_IN, TOK_OF,
	TOK_BREAK, TOK_CONTINUE, TOK_SWITCH, TOK_CASE, TOK_DEFAULT,
	TOK_TRY, TOK_CATCH, TOK_FINALLY, TOK_THROW,
	TOK_NEW, TOK_THIS, TOK_TYPEOF, TOK_INSTANCEOF, TOK_VOID, TOK_DELETE,
	TOK_TRUE, TOK_FALSE, TOK_NULL,
	TOK_CLASS, TOK_EXTENDS, TOK_SUPER, TOK_ASYNC, TOK_AWAIT,

	/* Punctuators */
	TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACK, TOK_RBRACK,
	TOK_DOT, TOK_OPTCHAIN, TOK_SPREAD, TOK_SEMI, TOK_COMMA, TOK_COLON, TOK_QMARK,
	TOK_ARROW,
	TOK_INC, TOK_DEC,
	TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_MOD, TOK_POW,
	TOK_BITAND, TOK_BITOR, TOK_BITXOR, TOK_BITNOT,
	TOK_SHL, TOK_SHR, TOK_USHR,
	TOK_NOT, TOK_AND, TOK_OR, TOK_NULLISH,
	TOK_EQ, TOK_NEQ, TOK_SEQ, TOK_SNEQ,
	TOK_LT, TOK_LE, TOK_GT, TOK_GE,
	TOK_ASSIGN,
	TOK_ADD_ASSIGN, TOK_SUB_ASSIGN, TOK_MUL_ASSIGN, TOK_DIV_ASSIGN,
	TOK_MOD_ASSIGN, TOK_POW_ASSIGN, TOK_AND_ASSIGN, TOK_OR_ASSIGN,
	TOK_XOR_ASSIGN, TOK_SHL_ASSIGN, TOK_SHR_ASSIGN, TOK_USHR_ASSIGN,
	TOK_LOGAND_ASSIGN, TOK_LOGOR_ASSIGN, TOK_NULLISH_ASSIGN
} js_tok_kind_t;

typedef struct {
	js_tok_kind_t kind;
	int line;
	int col;
	int nl_before;
	double num;
	char *text;
	size_t text_len;
	char *extra; /* regexp flags */
} js_tok_t;

typedef struct {
	const char *src;
	const char *end;
	const char *p;
	int line;
	const char *line_start;
	const char *filename;

	js_tok_t cur;
	js_tok_kind_t prev_kind;
	int had_error;
	char err_msg[256];

	/* Track template brace nesting depths so `}` inside ${ ... } resumes template string */
	int tmpl_depths[32];
	int tmpl_top;
	int brace_depth;
	int parse_depth;

	js_ast_pool_t *pool;
} js_parser_t;

static ipo_js_ast_t *ast_new(js_parser_t *ps, js_ast_kind_t k, int line, int col) {
	ipo_js_ast_t *n = (ipo_js_ast_t *)xcalloc(1, sizeof(ipo_js_ast_t));
	n->kind = k;
	n->line = line;
	n->col = col;
	if (ps->pool->len >= ps->pool->cap) {
		size_t ncap = ps->pool->cap ? ps->pool->cap * 2 : 64;
		ps->pool->nodes = (ipo_js_ast_t **)xrealloc(ps->pool->nodes, ncap * sizeof(ipo_js_ast_t *));
		ps->pool->cap = ncap;
	}
	ps->pool->nodes[ps->pool->len++] = n;
	return n;
}

static void parse_error(js_parser_t *ps, const char *fmt, ...) {
	if (ps->had_error) return;
	ps->had_error = 1;
	va_list ap;
	char msg[192];
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	snprintf(ps->err_msg, sizeof(ps->err_msg), "%s:%d:%d: %s",
	         ps->filename ? ps->filename : "<script>", ps->cur.line, ps->cur.col, msg);
}

static int lex_allows_regex(js_tok_kind_t pk) {
	switch (pk) {
	case TOK_EOF:
	case TOK_LPAREN: case TOK_LBRACE: case TOK_LBRACK:
	case TOK_COMMA: case TOK_SEMI: case TOK_COLON: case TOK_QMARK:
	case TOK_ARROW:
	case TOK_RETURN: case TOK_THROW: case TOK_CASE: case TOK_DEFAULT:
	case TOK_ELSE: case TOK_DO:
	case TOK_TYPEOF: case TOK_INSTANCEOF: case TOK_VOID: case TOK_DELETE:
	case TOK_IN: case TOK_OF: case TOK_NEW: case TOK_AWAIT:
	case TOK_PLUS: case TOK_MINUS: case TOK_MUL: case TOK_DIV: case TOK_MOD: case TOK_POW:
	case TOK_BITAND: case TOK_BITOR: case TOK_BITXOR: case TOK_BITNOT:
	case TOK_SHL: case TOK_SHR: case TOK_USHR:
	case TOK_NOT: case TOK_AND: case TOK_OR: case TOK_NULLISH:
	case TOK_EQ: case TOK_NEQ: case TOK_SEQ: case TOK_SNEQ:
	case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE:
	case TOK_ASSIGN: case TOK_ADD_ASSIGN: case TOK_SUB_ASSIGN:
	case TOK_MUL_ASSIGN: case TOK_DIV_ASSIGN: case TOK_MOD_ASSIGN:
	case TOK_POW_ASSIGN: case TOK_AND_ASSIGN: case TOK_OR_ASSIGN:
	case TOK_XOR_ASSIGN: case TOK_SHL_ASSIGN: case TOK_SHR_ASSIGN:
	case TOK_USHR_ASSIGN: case TOK_LOGAND_ASSIGN: case TOK_LOGOR_ASSIGN:
	case TOK_NULLISH_ASSIGN:
	case TOK_TMPL_HEAD: case TOK_TMPL_MID:
		return 1;
	default:
		return 0;
	}
}

static void lex_scan_escape(js_parser_t *ps, js_buf_t *b) {
	if (ps->p >= ps->end) return;
	char ch = *ps->p++;
	switch (ch) {
	case 'n': jbuf_append_ch(b, '\n'); break;
	case 'r': jbuf_append_ch(b, '\r'); break;
	case 't': jbuf_append_ch(b, '\t'); break;
	case 'b': jbuf_append_ch(b, '\b'); break;
	case 'f': jbuf_append_ch(b, '\f'); break;
	case 'v': jbuf_append_ch(b, '\v'); break;
	case '0': jbuf_append_ch(b, '\0'); break;
	case '\\': case '\'': case '"': case '`': case '$':
		jbuf_append_ch(b, ch); break;
	case '\r':
		if (ps->p < ps->end && *ps->p == '\n') ps->p++;
		ps->line++; ps->line_start = ps->p;
		break;
	case '\n':
		ps->line++; ps->line_start = ps->p;
		break;
	case 'x':
		if (ps->p + 1 < ps->end && isxdigit((unsigned char)ps->p[0]) && isxdigit((unsigned char)ps->p[1])) {
			int v = (re_hex_digit(ps->p[0]) << 4) | re_hex_digit(ps->p[1]);
			ps->p += 2;
			jbuf_append_ch(b, (char)v);
		} else {
			jbuf_append_ch(b, 'x');
		}
		break;
	case 'u':
		if (ps->p < ps->end && *ps->p == '{') {
			ps->p++;
			uint32_t cp = 0;
			while (ps->p < ps->end && isxdigit((unsigned char)*ps->p)) {
				cp = (cp << 4) | (uint32_t)re_hex_digit(*ps->p++);
			}
			if (ps->p < ps->end && *ps->p == '}') ps->p++;
			jbuf_append_utf8(b, cp);
		} else if (ps->p + 3 < ps->end && isxdigit((unsigned char)ps->p[0]) && isxdigit((unsigned char)ps->p[1]) &&
		           isxdigit((unsigned char)ps->p[2]) && isxdigit((unsigned char)ps->p[3])) {
			uint32_t cp = (uint32_t)((re_hex_digit(ps->p[0]) << 12) |
			                         (re_hex_digit(ps->p[1]) << 8) |
			                         (re_hex_digit(ps->p[2]) << 4) |
			                         re_hex_digit(ps->p[3]));
			ps->p += 4;
			jbuf_append_utf8(b, cp);
		} else {
			jbuf_append_ch(b, 'u');
		}
		break;
	default:
		jbuf_append_ch(b, ch);
		break;
	}
}
/* Note: remove unused helper reference */
#undef c_end_check
static const char *dummy_unused_fix_helper(js_parser_t *ps) { return ps->end; }

/* Clean rewrite without placeholder macro above */
static void lex_scan_escape_clean(js_parser_t *ps, js_buf_t *b) {
	(void)dummy_unused_fix_helper;
	if (ps->p >= ps->end) return;
	char ch = *ps->p++;
	switch (ch) {
	case 'n': jbuf_append_ch(b, '\n'); break;
	case 'r': jbuf_append_ch(b, '\r'); break;
	case 't': jbuf_append_ch(b, '\t'); break;
	case 'b': jbuf_append_ch(b, '\b'); break;
	case 'f': jbuf_append_ch(b, '\f'); break;
	case 'v': jbuf_append_ch(b, '\v'); break;
	case '0': jbuf_append_ch(b, '\0'); break;
	case '\\': case '\'': case '"': case '`': case '$':
		jbuf_append_ch(b, ch); break;
	case '\r':
		if (ps->p < ps->end && *ps->p == '\n') ps->p++;
		ps->line++; ps->line_start = ps->p;
		break;
	case '\n':
		ps->line++; ps->line_start = ps->p;
		break;
	case 'x':
		if (ps->p + 1 < ps->end && isxdigit((unsigned char)ps->p[0]) && isxdigit((unsigned char)ps->p[1])) {
			int v = (re_hex_digit(ps->p[0]) << 4) | re_hex_digit(ps->p[1]);
			ps->p += 2;
			jbuf_append_ch(b, (char)v);
		} else {
			jbuf_append_ch(b, 'x');
		}
		break;
	case 'u':
		if (ps->p < ps->end && *ps->p == '{') {
			ps->p++;
			uint32_t cp = 0;
			while (ps->p < ps->end && isxdigit((unsigned char)*ps->p)) {
				cp = (cp << 4) | (uint32_t)re_hex_digit(*ps->p++);
			}
			if (ps->p < ps->end && *ps->p == '}') ps->p++;
			jbuf_append_utf8(b, cp);
		} else if (ps->p + 3 < ps->end && isxdigit((unsigned char)ps->p[0]) && isxdigit((unsigned char)ps->p[1]) &&
		           isxdigit((unsigned char)ps->p[2]) && isxdigit((unsigned char)ps->p[3])) {
			uint32_t cp = (uint32_t)((re_hex_digit(ps->p[0]) << 12) |
			                         (re_hex_digit(ps->p[1]) << 8) |
			                         (re_hex_digit(ps->p[2]) << 4) |
			                         re_hex_digit(ps->p[3]));
			ps->p += 4;
			jbuf_append_utf8(b, cp);
		} else {
			jbuf_append_ch(b, 'u');
		}
		break;
	default:
		jbuf_append_ch(b, ch);
		break;
	}
}

static void lex_scan_template(js_parser_t *ps, int is_head) {
	js_buf_t b;
	jbuf_init(&b);
	while (ps->p < ps->end) {
		char ch = *ps->p++;
		if (ch == '`') {
			ps->cur.kind = is_head ? TOK_TMPL_FULL : TOK_TMPL_TAIL;
			ps->cur.text = b.data ? b.data : xstrdup_n("", 0);
			ps->cur.text_len = b.len;
			return;
		}
		if (ch == '$' && ps->p < ps->end && *ps->p == '{') {
			ps->p++;
			ps->cur.kind = is_head ? TOK_TMPL_HEAD : TOK_TMPL_MID;
			ps->cur.text = b.data ? b.data : xstrdup_n("", 0);
			ps->cur.text_len = b.len;
			if (ps->tmpl_top < 32) ps->tmpl_depths[ps->tmpl_top++] = ps->brace_depth;
			ps->brace_depth++;
			return;
		}
		if (ch == '\\') {
			lex_scan_escape_clean(ps, &b);
		} else {
			if (ch == '\r') {
				if (ps->p < ps->end && *ps->p == '\n') ps->p++;
				ps->line++; ps->line_start = ps->p;
				jbuf_append_ch(&b, '\n');
			} else {
				if (ch == '\n') { ps->line++; ps->line_start = ps->p; }
				jbuf_append_ch(&b, ch);
			}
		}
	}
	jbuf_free(&b);
	parse_error(ps, "Unterminated template literal");
	ps->cur.kind = TOK_EOF;
}

static js_tok_kind_t check_keyword(const char *s, size_t len) {
	struct kw { const char *w; size_t l; js_tok_kind_t k; };
	static const struct kw kws[] = {
		{"var", 3, TOK_VAR}, {"let", 3, TOK_LET}, {"const", 5, TOK_CONST},
		{"function", 8, TOK_FUNCTION}, {"return", 6, TOK_RETURN},
		{"if", 2, TOK_IF}, {"else", 4, TOK_ELSE}, {"while", 5, TOK_WHILE},
		{"do", 2, TOK_DO}, {"for", 3, TOK_FOR}, {"in", 2, TOK_IN}, {"of", 2, TOK_OF},
		{"break", 5, TOK_BREAK}, {"continue", 8, TOK_CONTINUE},
		{"switch", 6, TOK_SWITCH}, {"case", 4, TOK_CASE}, {"default", 7, TOK_DEFAULT},
		{"try", 3, TOK_TRY}, {"catch", 5, TOK_CATCH}, {"finally", 7, TOK_FINALLY},
		{"throw", 5, TOK_THROW}, {"new", 3, TOK_NEW}, {"this", 4, TOK_THIS},
		{"typeof", 6, TOK_TYPEOF}, {"instanceof", 10, TOK_INSTANCEOF},
		{"void", 4, TOK_VOID}, {"delete", 6, TOK_DELETE},
		{"true", 4, TOK_TRUE}, {"false", 5, TOK_FALSE}, {"null", 4, TOK_NULL},
		{"class", 5, TOK_CLASS}, {"extends", 7, TOK_EXTENDS}, {"super", 5, TOK_SUPER},
		{"async", 5, TOK_ASYNC}, {"await", 5, TOK_AWAIT}
	};
	for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
		if (len == kws[i].l && memcmp(s, kws[i].w, len) == 0) return kws[i].k;
	}
	return TOK_IDENT;
}

static int is_ident_start(unsigned char c) {
	return isalpha(c) || c == '_' || c == '$' || c >= 0x80;
}

static int is_ident_cont(unsigned char c) {
	return isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

static void lex_next(js_parser_t *ps) {
	ps->prev_kind = ps->cur.kind;
	free(ps->cur.text);  ps->cur.text = NULL;
	free(ps->cur.extra); ps->cur.extra = NULL;
	ps->cur.text_len = 0;
	ps->cur.num = 0;

	int nl = 0;
	/* Skip whitespace & comments */
	for (;;) {
		while (ps->p < ps->end) {
			unsigned char c = (unsigned char)*ps->p;
			if (c == '\n') { nl = 1; ps->p++; ps->line++; ps->line_start = ps->p; }
			else if (c == '\r') { nl = 1; ps->p++; if (ps->p < ps->end && *ps->p == '\n') ps->p++; ps->line++; ps->line_start = ps->p; }
			else if (c == ' ' || c == '\t' || c == '\f' || c == '\v') ps->p++;
			else break;
		}
		/* HTML-style <!-- comment only at start of line or file */
		if (ps->p + 1 < ps->end && ps->p[0] == '/' && ps->p[1] == '/') {
			ps->p += 2;
			while (ps->p < ps->end && *ps->p != '\n' && *ps->p != '\r') ps->p++;
			continue;
		}
		if (ps->p + 1 < ps->end && ps->p[0] == '/' && ps->p[1] == '*') {
			ps->p += 2;
			while (ps->p + 1 < ps->end && !(ps->p[0] == '*' && ps->p[1] == '/')) {
				if (*ps->p == '\n') { nl = 1; ps->line++; ps->line_start = ps->p + 1; }
				else if (*ps->p == '\r') { nl = 1; }
				ps->p++;
			}
			if (ps->p + 1 < ps->end) ps->p += 2;
			else ps->p = ps->end;
			continue;
		}
		if (ps->p == ps->src && ps->p + 1 < ps->end && ps->p[0] == '#' && ps->p[1] == '!') {
			while (ps->p < ps->end && *ps->p != '\n') ps->p++;
			continue;
		}
		break;
	}

	ps->cur.nl_before = nl;
	ps->cur.line = ps->line;
	ps->cur.col = (int)(ps->p - ps->line_start) + 1;

	if (ps->p >= ps->end) {
		ps->cur.kind = TOK_EOF;
		return;
	}

	char c = *ps->p;

	/* Template literal middle/tail continuation after '}' */
	if (c == '}' && ps->tmpl_top > 0 && ps->brace_depth - 1 == ps->tmpl_depths[ps->tmpl_top - 1]) {
		ps->p++;
		ps->brace_depth--;
		ps->tmpl_top--;
		lex_scan_template(ps, 0);
		return;
	}

	/* String literal */
	if (c == '\'' || c == '"') {
		char quote = c;
		ps->p++;
		js_buf_t b;
		jbuf_init(&b);
		while (ps->p < ps->end && *ps->p != quote) {
			if (*ps->p == '\n' || *ps->p == '\r') break;
			if (*ps->p == '\\') {
				ps->p++;
				lex_scan_escape_clean(ps, &b);
			} else {
				jbuf_append_ch(&b, *ps->p++);
			}
		}
		if (ps->p < ps->end && *ps->p == quote) ps->p++;
		else parse_error(ps, "Unterminated string literal");
		ps->cur.kind = TOK_STR;
		ps->cur.text = b.data ? b.data : xstrdup_n("", 0);
		ps->cur.text_len = b.len;
		return;
	}

	if (c == '`') {
		ps->p++;
		lex_scan_template(ps, 1);
		return;
	}

	/* Number literal */
	if (isdigit((unsigned char)c) || (c == '.' && ps->p + 1 < ps->end && isdigit((unsigned char)ps->p[1]))) {
		const char *start = ps->p;
		if (c == '0' && ps->p + 1 < ps->end && (ps->p[1] == 'x' || ps->p[1] == 'X' ||
		    ps->p[1] == 'b' || ps->p[1] == 'B' || ps->p[1] == 'o' || ps->p[1] == 'O')) {
			char base_ch = ps->p[1];
			ps->p += 2;
			uint64_t val = 0;
			int base = (base_ch == 'x' || base_ch == 'X') ? 16 : ((base_ch == 'b' || base_ch == 'B') ? 2 : 8);
			while (ps->p < ps->end) {
				char d = *ps->p;
				if (d == '_') { ps->p++; continue; }
				int dv = re_hex_digit(d);
				if (dv < 0 || dv >= base) break;
				val = val * (uint64_t)base + (uint64_t)dv;
				ps->p++;
			}
			ps->cur.kind = TOK_NUM;
			ps->cur.num = (double)val;
			return;
		}
		js_buf_t nb;
		jbuf_init(&nb);
		while (ps->p < ps->end && (isdigit((unsigned char)*ps->p) || *ps->p == '_')) {
			if (*ps->p != '_') jbuf_append_ch(&nb, *ps->p);
			ps->p++;
		}
		if (ps->p < ps->end && *ps->p == '.') {
			jbuf_append_ch(&nb, '.');
			ps->p++;
			while (ps->p < ps->end && (isdigit((unsigned char)*ps->p) || *ps->p == '_')) {
				if (*ps->p != '_') jbuf_append_ch(&nb, *ps->p);
				ps->p++;
			}
		}
		if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
			jbuf_append_ch(&nb, 'e');
			ps->p++;
			if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) jbuf_append_ch(&nb, *ps->p++);
			while (ps->p < ps->end && (isdigit((unsigned char)*ps->p) || *ps->p == '_')) {
				if (*ps->p != '_') jbuf_append_ch(&nb, *ps->p);
				ps->p++;
			}
		}
		ps->cur.kind = TOK_NUM;
		ps->cur.num = nb.data ? strtod(nb.data, NULL) : 0.0;
		jbuf_free(&nb);
		(void)start;
		return;
	}

	/* RegExp literal /.../flags */
	if (c == '/' && lex_allows_regex(ps->prev_kind)) {
		ps->p++;
		js_buf_t pat;
		jbuf_init(&pat);
		int in_cls = 0;
		while (ps->p < ps->end) {
			char rc = *ps->p++;
			if (rc == '\n' || rc == '\r') {
				parse_error(ps, "Unterminated regular expression literal");
				break;
			}
			if (rc == '\\' && ps->p < ps->end) {
				jbuf_append_ch(&pat, '\\');
				jbuf_append_ch(&pat, *ps->p++);
				continue;
			}
			if (rc == '[') in_cls = 1;
			else if (rc == ']') in_cls = 0;
			else if (rc == '/' && !in_cls) break;
			jbuf_append_ch(&pat, rc);
		}
		const char *fstart = ps->p;
		while (ps->p < ps->end && isalpha((unsigned char)*ps->p)) ps->p++;
		ps->cur.kind = TOK_REGEX;
		ps->cur.text = pat.data ? pat.data : xstrdup_n("", 0);
		ps->cur.text_len = pat.len;
		ps->cur.extra = xstrdup_n(fstart, (size_t)(ps->p - fstart));
		return;
	}

	/* Identifier or keyword */
	if (is_ident_start((unsigned char)c)) {
		const char *start = ps->p;
		ps->p++;
		while (ps->p < ps->end && is_ident_cont((unsigned char)*ps->p)) ps->p++;
		size_t ilen = (size_t)(ps->p - start);
		ps->cur.kind = check_keyword(start, ilen);
		ps->cur.text = xstrdup_n(start, ilen);
		ps->cur.text_len = ilen;
		return;
	}

	/* Multi-char operators */
	ps->p++;
	switch (c) {
	case '(': ps->cur.kind = TOK_LPAREN; return;
	case ')': ps->cur.kind = TOK_RPAREN; return;
	case '{': ps->brace_depth++; ps->cur.kind = TOK_LBRACE; return;
	case '}': if (ps->brace_depth > 0) ps->brace_depth--; ps->cur.kind = TOK_RBRACE; return;
	case '[': ps->cur.kind = TOK_LBRACK; return;
	case ']': ps->cur.kind = TOK_RBRACK; return;
	case ';': ps->cur.kind = TOK_SEMI; return;
	case ',': ps->cur.kind = TOK_COMMA; return;
	case ':': ps->cur.kind = TOK_COLON; return;
	case '~': ps->cur.kind = TOK_BITNOT; return;
	case '.':
		if (ps->p + 1 < ps->end && ps->p[0] == '.' && ps->p[1] == '.') {
			ps->p += 2; ps->cur.kind = TOK_SPREAD;
		} else {
			ps->cur.kind = TOK_DOT;
		}
		return;
	case '?':
		if (ps->p < ps->end && *ps->p == '?') {
			ps->p++;
			if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_NULLISH_ASSIGN; }
			else ps->cur.kind = TOK_NULLISH;
		} else if (ps->p < ps->end && *ps->p == '.' &&
		           !(ps->p + 1 < ps->end && isdigit((unsigned char)ps->p[1]))) {
			ps->p++; ps->cur.kind = TOK_OPTCHAIN;
		} else {
			ps->cur.kind = TOK_QMARK;
		}
		return;
	case '+':
		if (ps->p < ps->end && *ps->p == '+') { ps->p++; ps->cur.kind = TOK_INC; }
		else if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_ADD_ASSIGN; }
		else ps->cur.kind = TOK_PLUS;
		return;
	case '-':
		if (ps->p < ps->end && *ps->p == '-') { ps->p++; ps->cur.kind = TOK_DEC; }
		else if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_SUB_ASSIGN; }
		else ps->cur.kind = TOK_MINUS;
		return;
	case '*':
		if (ps->p < ps->end && *ps->p == '*') {
			ps->p++;
			if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_POW_ASSIGN; }
			else ps->cur.kind = TOK_POW;
		} else if (ps->p < ps->end && *ps->p == '=') {
			ps->p++; ps->cur.kind = TOK_MUL_ASSIGN;
		} else {
			ps->cur.kind = TOK_MUL;
		}
		return;
	case '/':
		if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_DIV_ASSIGN; }
		else ps->cur.kind = TOK_DIV;
		return;
	case '%':
		if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_MOD_ASSIGN; }
		else ps->cur.kind = TOK_MOD;
		return;
	case '=':
		if (ps->p < ps->end && *ps->p == '>') { ps->p++; ps->cur.kind = TOK_ARROW; }
		else if (ps->p + 1 < ps->end && ps->p[0] == '=' && ps->p[1] == '=') { ps->p += 2; ps->cur.kind = TOK_SEQ; }
		else if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_EQ; }
		else ps->cur.kind = TOK_ASSIGN;
		return;
	case '!':
		if (ps->p + 1 < ps->end && ps->p[0] == '=' && ps->p[1] == '=') { ps->p += 2; ps->cur.kind = TOK_SNEQ; }
		else if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_NEQ; }
		else ps->cur.kind = TOK_NOT;
		return;
	case '<':
		if (ps->p < ps->end && *ps->p == '<') {
			ps->p++;
			if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_SHL_ASSIGN; }
			else ps->cur.kind = TOK_SHL;
		} else if (ps->p < ps->end && *ps->p == '=') {
			ps->p++; ps->cur.kind = TOK_LE;
		} else ps->cur.kind = TOK_LT;
		return;
	case '>':
		if (ps->p + 1 < ps->end && ps->p[0] == '>' && ps->p[1] == '>') {
			ps->p += 2;
			if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_USHR_ASSIGN; }
			else ps->cur.kind = TOK_USHR;
		} else if (ps->p < ps->end && *ps->p == '>') {
			ps->p++;
			if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_SHR_ASSIGN; }
			else ps->cur.kind = TOK_SHR;
		} else if (ps->p < ps->end && *ps->p == '=') {
			ps->p++; ps->cur.kind = TOK_GE;
		} else ps->cur.kind = TOK_GT;
		return;
	case '&':
		if (ps->p < ps->end && *ps->p == '&') {
			ps->p++;
			if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_LOGAND_ASSIGN; }
			else ps->cur.kind = TOK_AND;
		} else if (ps->p < ps->end && *ps->p == '=') {
			ps->p++; ps->cur.kind = TOK_AND_ASSIGN;
		} else ps->cur.kind = TOK_BITAND;
		return;
	case '|':
		if (ps->p < ps->end && *ps->p == '|') {
			ps->p++;
			if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_LOGOR_ASSIGN; }
			else ps->cur.kind = TOK_OR;
		} else if (ps->p < ps->end && *ps->p == '=') {
			ps->p++; ps->cur.kind = TOK_OR_ASSIGN;
		} else ps->cur.kind = TOK_BITOR;
		return;
	case '^':
		if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = TOK_XOR_ASSIGN; }
		else ps->cur.kind = TOK_BITXOR;
		return;
	default:
		parse_error(ps, "Unexpected character '%c'", c);
		ps->cur.kind = TOK_EOF;
		return;
	}
}

static int tok_is_ident_or_kw(js_tok_kind_t k) {
	return k == TOK_IDENT || (k >= TOK_VAR && k <= TOK_AWAIT);
}

static void expect(js_parser_t *ps, js_tok_kind_t k, const char *what) {
	if (ps->had_error) return;
	if (ps->cur.kind != k) {
		parse_error(ps, "Expected %s", what);
		return;
	}
	lex_next(ps);
}

static void consume_semi(js_parser_t *ps) {
	if (ps->had_error) return;
	if (ps->cur.kind == TOK_SEMI) {
		lex_next(ps);
		return;
	}
	if (ps->cur.kind == TOK_RBRACE || ps->cur.kind == TOK_EOF || ps->cur.nl_before) return;
	parse_error(ps, "Expected ';'");
}

static void ast_list_push(js_ast_list_t *l, ipo_js_ast_t *n) {
	if (l->len >= l->cap) {
		size_t ncap = l->cap ? l->cap * 2 : 4;
		l->items = (ipo_js_ast_t **)xrealloc(l->items, ncap * sizeof(ipo_js_ast_t *));
		l->cap = ncap;
	}
	l->items[l->len++] = n;
}

static ipo_js_ast_t *parse_expr(js_parser_t *ps);
static ipo_js_ast_t *parse_assign(js_parser_t *ps);
static ipo_js_ast_t *parse_stmt(js_parser_t *ps);
static ipo_js_ast_t *parse_block(js_parser_t *ps);
static ipo_js_ast_t *parse_function(js_parser_t *ps, int is_decl, int is_async);
static ipo_js_ast_t *parse_class(js_parser_t *ps, int is_decl);

static ipo_js_ast_t *parse_pattern(js_parser_t *ps) {
	if (ps->had_error) return NULL;
	int ln = ps->cur.line, cl = ps->cur.col;
	if (ps->cur.kind == TOK_IDENT || ps->cur.kind == TOK_ASYNC || ps->cur.kind == TOK_OF) {
		ipo_js_ast_t *n = ast_new(ps, AST_IDENT, ln, cl);
		n->u.ident.name = xstrdup_n(ps->cur.text, ps->cur.text_len);
		lex_next(ps);
		return n;
	}
	if (ps->cur.kind == TOK_LBRACK) {
		lex_next(ps);
		ipo_js_ast_t *p = ast_new(ps, AST_ARR_PAT, ln, cl);
		size_t cap = 0;
		while (!ps->had_error && ps->cur.kind != TOK_RBRACK && ps->cur.kind != TOK_EOF) {
			if (p->u.pat.len >= cap) {
				cap = cap ? cap * 2 : 4;
				p->u.pat.elems = (js_pat_prop_t *)xrealloc(p->u.pat.elems, cap * sizeof(js_pat_prop_t));
			}
			js_pat_prop_t *el = &p->u.pat.elems[p->u.pat.len++];
			memset(el, 0, sizeof(*el));
			if (ps->cur.kind == TOK_COMMA) {
				lex_next(ps);
				continue;
			}
			if (ps->cur.kind == TOK_SPREAD) {
				el->is_rest = 1;
				lex_next(ps);
			}
			el->target = parse_pattern(ps);
			if (!el->is_rest && ps->cur.kind == TOK_ASSIGN) {
				lex_next(ps);
				el->def_val = parse_assign(ps);
			}
			if (ps->cur.kind == TOK_COMMA) lex_next(ps);
			else break;
		}
		expect(ps, TOK_RBRACK, "']'");
		return p;
	}
	if (ps->cur.kind == TOK_LBRACE) {
		lex_next(ps);
		ipo_js_ast_t *p = ast_new(ps, AST_OBJ_PAT, ln, cl);
		size_t cap = 0;
		while (!ps->had_error && ps->cur.kind != TOK_RBRACE && ps->cur.kind != TOK_EOF) {
			if (p->u.pat.len >= cap) {
				cap = cap ? cap * 2 : 4;
				p->u.pat.elems = (js_pat_prop_t *)xrealloc(p->u.pat.elems, cap * sizeof(js_pat_prop_t));
			}
			js_pat_prop_t *el = &p->u.pat.elems[p->u.pat.len++];
			memset(el, 0, sizeof(*el));
			if (ps->cur.kind == TOK_SPREAD) {
				el->is_rest = 1;
				lex_next(ps);
				el->target = parse_pattern(ps);
			} else {
				if (ps->cur.kind == TOK_LBRACK) {
					lex_next(ps);
					el->key_ast = parse_assign(ps);
					expect(ps, TOK_RBRACK, "']'");
					expect(ps, TOK_COLON, "':'");
					el->target = parse_pattern(ps);
				} else if (ps->cur.kind == TOK_STR) {
					el->key_name = xstrdup_n(ps->cur.text, ps->cur.text_len);
					lex_next(ps);
					expect(ps, TOK_COLON, "':'");
					el->target = parse_pattern(ps);
				} else if (tok_is_ident_or_kw(ps->cur.kind)) {
					el->key_name = xstrdup_n(ps->cur.text, ps->cur.text_len);
					int kl = ps->cur.line, kc = ps->cur.col;
					lex_next(ps);
					if (ps->cur.kind == TOK_COLON) {
						lex_next(ps);
						el->target = parse_pattern(ps);
					} else {
						ipo_js_ast_t *id = ast_new(ps, AST_IDENT, kl, kc);
						id->u.ident.name = xstrdup_n(el->key_name, strlen(el->key_name));
						el->target = id;
					}
				} else {
					parse_error(ps, "Invalid destructuring key");
					return p;
				}
				if (ps->cur.kind == TOK_ASSIGN) {
					lex_next(ps);
					el->def_val = parse_assign(ps);
				}
			}
			if (ps->cur.kind == TOK_COMMA) lex_next(ps);
			else break;
		}
		expect(ps, TOK_RBRACE, "'}'");
		return p;
	}
	parse_error(ps, "Expected identifier or destructuring pattern");
	return NULL;
}

static void parse_param_list(js_parser_t *ps, js_param_ast_t **out_p, size_t *out_n) {
	expect(ps, TOK_LPAREN, "'('");
	js_param_ast_t *params = NULL;
	size_t len = 0, cap = 0;
	while (!ps->had_error && ps->cur.kind != TOK_RPAREN && ps->cur.kind != TOK_EOF) {
		if (len >= cap) {
			cap = cap ? cap * 2 : 4;
			params = (js_param_ast_t *)xrealloc(params, cap * sizeof(js_param_ast_t));
		}
		js_param_ast_t *pa = &params[len++];
		memset(pa, 0, sizeof(*pa));
		if (ps->cur.kind == TOK_SPREAD) {
			pa->is_rest = 1;
			lex_next(ps);
		}
		pa->pattern = parse_pattern(ps);
		if (!pa->is_rest && ps->cur.kind == TOK_ASSIGN) {
			lex_next(ps);
			pa->def_val = parse_assign(ps);
		}
		if (ps->cur.kind == TOK_COMMA) lex_next(ps);
		else break;
	}
	expect(ps, TOK_RPAREN, "')'");
	*out_p = params;
	*out_n = len;
}

static int try_is_arrow_params(js_parser_t *ps) {
	if (ps->cur.kind == TOK_IDENT && ps->p < ps->end) {
		const char *q = ps->p;
		while (q < ps->end && (*q == ' ' || *q == '\t')) q++;
		return (q + 1 < ps->end && q[0] == '=' && q[1] == '>');
	}
	if (ps->cur.kind == TOK_LPAREN) {
		/* Scan matching ')' to check for '=>' without consuming tokens */
		const char *q = ps->p;
		int depth = 1;
		while (q < ps->end && depth > 0) {
			char ch = *q++;
			if (ch == '\'' || ch == '"' || ch == '`') {
				while (q < ps->end && *q != ch) {
					if (*q == '\\' && q + 1 < ps->end) q++;
					q++;
				}
				if (q < ps->end) q++;
				continue;
			}
			if (ch == '/' && q < ps->end && *q == '/') {
				while (q < ps->end && *q != '\n') q++;
				continue;
			}
			if (ch == '/' && q < ps->end && *q == '*') {
				q++;
				while (q + 1 < ps->end && !(q[0] == '*' && q[1] == '/')) q++;
				if (q + 1 < ps->end) q += 2;
				continue;
			}
			if (ch == '(') depth++;
			else if (ch == ')') depth--;
		}
		if (depth != 0) return 0;
		while (q < ps->end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
		return (q + 1 < ps->end && q[0] == '=' && q[1] == '>');
	}
	return 0;
}

static ipo_js_ast_t *parse_arrow_fn(js_parser_t *ps, int is_async) {
	int ln = ps->cur.line, cl = ps->cur.col;
	ipo_js_ast_t *fn = ast_new(ps, AST_FUNC, ln, cl);
	fn->u.fn.is_arrow = 1;
	fn->u.fn.is_async = is_async;
	if (ps->cur.kind == TOK_IDENT) {
		js_param_ast_t *p = (js_param_ast_t *)xcalloc(1, sizeof(js_param_ast_t));
		ipo_js_ast_t *id = ast_new(ps, AST_IDENT, ln, cl);
		id->u.ident.name = xstrdup_n(ps->cur.text, ps->cur.text_len);
		p->pattern = id;
		fn->u.fn.params = p;
		fn->u.fn.param_len = 1;
		lex_next(ps);
	} else {
		parse_param_list(ps, &fn->u.fn.params, &fn->u.fn.param_len);
	}
	expect(ps, TOK_ARROW, "'=>'");
	if (ps->cur.kind == TOK_LBRACE) {
		fn->u.fn.body = parse_block(ps);
	} else {
		ipo_js_ast_t *blk = ast_new(ps, AST_BLOCK, ps->cur.line, ps->cur.col);
		ipo_js_ast_t *ret = ast_new(ps, AST_RETURN, ps->cur.line, ps->cur.col);
		ret->u.expr_stmt.expr = parse_assign(ps);
		ast_list_push(&blk->u.block.stmts, ret);
		fn->u.fn.body = blk;
	}
	return fn;
}

static ipo_js_ast_t *parse_primary_inner(js_parser_t *ps);
static ipo_js_ast_t *parse_primary(js_parser_t *ps) {
	if (ps->had_error) return NULL;
	if (ps->parse_depth >= 128) {
		parse_error(ps, "Maximum parse nesting depth exceeded");
		return NULL;
	}
	ps->parse_depth++;
	ipo_js_ast_t *r = parse_primary_inner(ps);
	ps->parse_depth--;
	return r;
}
static ipo_js_ast_t *parse_primary_inner(js_parser_t *ps) {
	if (ps->had_error) return NULL;
	int ln = ps->cur.line, cl = ps->cur.col;

	/* async function or async arrow */
	if (ps->cur.kind == TOK_ASYNC) {
		const char *q = ps->p;
		while (q < ps->end && (*q == ' ' || *q == '\t')) q++;
		if (strncmp(q, "function", 8) == 0 && (q + 8 >= ps->end || !is_ident_cont((unsigned char)q[8]))) {
			lex_next(ps);
			return parse_function(ps, 0, 1);
		}
		if (*q == '(' || is_ident_start((unsigned char)*q)) {
			/* Look for '=>' after single ident or (...) */
			lex_next(ps);
			if (try_is_arrow_params(ps)) return parse_arrow_fn(ps, 1);
			parse_error(ps, "Unexpected 'async'");
			return NULL;
		}
	}

	if (try_is_arrow_params(ps)) {
		return parse_arrow_fn(ps, 0);
	}

	switch (ps->cur.kind) {
	case TOK_NULL:
		lex_next(ps); return ast_new(ps, AST_NULL, ln, cl);
	case TOK_TRUE:
	case TOK_FALSE: {
		ipo_js_ast_t *n = ast_new(ps, AST_BOOL, ln, cl);
		n->u.b_lit.b = (ps->cur.kind == TOK_TRUE);
		lex_next(ps);
		return n;
	}
	case TOK_THIS:
		lex_next(ps); return ast_new(ps, AST_THIS, ln, cl);
	case TOK_SUPER:
		lex_next(ps); return ast_new(ps, AST_SUPER, ln, cl);
	case TOK_NUM: {
		ipo_js_ast_t *n = ast_new(ps, AST_NUM, ln, cl);
		n->u.n_lit.num = ps->cur.num;
		lex_next(ps);
		return n;
	}
	case TOK_STR: {
		ipo_js_ast_t *n = ast_new(ps, AST_STR, ln, cl);
		n->u.s_lit.str = xstrdup_n(ps->cur.text, ps->cur.text_len);
		n->u.s_lit.len = ps->cur.text_len;
		lex_next(ps);
		return n;
	}
	case TOK_REGEX: {
		ipo_js_ast_t *n = ast_new(ps, AST_REGEXP, ln, cl);
		n->u.re_lit.pat = xstrdup_n(ps->cur.text, ps->cur.text_len);
		n->u.re_lit.flags = xstrdup_n(ps->cur.extra ? ps->cur.extra : "",
		                              ps->cur.extra ? strlen(ps->cur.extra) : 0);
		lex_next(ps);
		return n;
	}
	case TOK_TMPL_FULL: {
		ipo_js_ast_t *n = ast_new(ps, AST_STR, ln, cl);
		n->u.s_lit.str = xstrdup_n(ps->cur.text, ps->cur.text_len);
		n->u.s_lit.len = ps->cur.text_len;
		lex_next(ps);
		return n;
	}
	case TOK_TMPL_HEAD: {
		ipo_js_ast_t *n = ast_new(ps, AST_TEMPLATE, ln, cl);
		size_t cap = 4;
		n->u.tmpl.quasis = (char **)xmalloc((cap + 1) * sizeof(char *));
		n->u.tmpl.qlen   = (size_t *)xmalloc((cap + 1) * sizeof(size_t));
		n->u.tmpl.exprs  = (ipo_js_ast_t **)xmalloc(cap * sizeof(ipo_js_ast_t *));
		n->u.tmpl.quasis[0] = xstrdup_n(ps->cur.text, ps->cur.text_len);
		n->u.tmpl.qlen[0]   = ps->cur.text_len;
		lex_next(ps);
		for (;;) {
			ipo_js_ast_t *ex = parse_expr(ps);
			if (n->u.tmpl.count >= cap) {
				cap *= 2;
				n->u.tmpl.quasis = (char **)xrealloc(n->u.tmpl.quasis, (cap + 1) * sizeof(char *));
				n->u.tmpl.qlen   = (size_t *)xrealloc(n->u.tmpl.qlen, (cap + 1) * sizeof(size_t));
				n->u.tmpl.exprs  = (ipo_js_ast_t **)xrealloc(n->u.tmpl.exprs, cap * sizeof(ipo_js_ast_t *));
			}
			n->u.tmpl.exprs[n->u.tmpl.count++] = ex;
			if (ps->cur.kind == TOK_TMPL_MID) {
				n->u.tmpl.quasis[n->u.tmpl.count] = xstrdup_n(ps->cur.text, ps->cur.text_len);
				n->u.tmpl.qlen[n->u.tmpl.count]   = ps->cur.text_len;
				lex_next(ps);
			} else if (ps->cur.kind == TOK_TMPL_TAIL) {
				n->u.tmpl.quasis[n->u.tmpl.count] = xstrdup_n(ps->cur.text, ps->cur.text_len);
				n->u.tmpl.qlen[n->u.tmpl.count]   = ps->cur.text_len;
				lex_next(ps);
				break;
			} else {
				n->u.tmpl.quasis[n->u.tmpl.count] = xstrdup_n("", 0);
				n->u.tmpl.qlen[n->u.tmpl.count]   = 0;
				parse_error(ps, "Expected closing template backtick");
				break;
			}
		}
		return n;
	}
	case TOK_IDENT:
	case TOK_ASYNC:
	case TOK_OF: {
		ipo_js_ast_t *n = ast_new(ps, AST_IDENT, ln, cl);
		n->u.ident.name = xstrdup_n(ps->cur.text, ps->cur.text_len);
		lex_next(ps);
		return n;
	}
	case TOK_FUNCTION:
		return parse_function(ps, 0, 0);
	case TOK_CLASS:
		return parse_class(ps, 0);
	case TOK_LPAREN: {
		lex_next(ps);
		ipo_js_ast_t *e = parse_expr(ps);
		expect(ps, TOK_RPAREN, "')'");
		return e;
	}
	case TOK_LBRACK: {
		lex_next(ps);
		ipo_js_ast_t *arr = ast_new(ps, AST_ARRAY, ln, cl);
		while (!ps->had_error && ps->cur.kind != TOK_RBRACK && ps->cur.kind != TOK_EOF) {
			if (ps->cur.kind == TOK_COMMA) {
				ast_list_push(&arr->u.arr.elems, ast_new(ps, AST_UNDEF, ps->cur.line, ps->cur.col));
				lex_next(ps);
				continue;
			}
			if (ps->cur.kind == TOK_SPREAD) {
				int sl = ps->cur.line, sc = ps->cur.col;
				lex_next(ps);
				ipo_js_ast_t *sp = ast_new(ps, AST_SPREAD, sl, sc);
				sp->u.spread.arg = parse_assign(ps);
				ast_list_push(&arr->u.arr.elems, sp);
			} else {
				ast_list_push(&arr->u.arr.elems, parse_assign(ps));
			}
			if (ps->cur.kind == TOK_COMMA) lex_next(ps);
			else break;
		}
		expect(ps, TOK_RBRACK, "']'");
		return arr;
	}
	case TOK_LBRACE: {
		lex_next(ps);
		ipo_js_ast_t *obj = ast_new(ps, AST_OBJECT, ln, cl);
		size_t cap = 0;
		while (!ps->had_error && ps->cur.kind != TOK_RBRACE && ps->cur.kind != TOK_EOF) {
			if (obj->u.obj.len >= cap) {
				cap = cap ? cap * 2 : 4;
				obj->u.obj.props = (js_obj_prop_ast_t *)xrealloc(obj->u.obj.props,
				                                                 cap * sizeof(js_obj_prop_ast_t));
			}
			js_obj_prop_ast_t *pr = &obj->u.obj.props[obj->u.obj.len++];
			memset(pr, 0, sizeof(*pr));
			if (ps->cur.kind == TOK_SPREAD) {
				pr->kind = 3;
				lex_next(ps);
				pr->val = parse_assign(ps);
			} else {
				int prop_kind = 0;
				int prop_async = 0;
				if (ps->cur.kind == TOK_ASYNC) {
					const char *q = ps->p;
					while (q < ps->end && (*q == ' ' || *q == '\t')) q++;
					if (*q != ':' && *q != ',' && *q != '(' && *q != '}') {
						prop_async = 1;
						lex_next(ps);
					}
				}
				if ((ps->cur.kind == TOK_IDENT) &&
				    (strcmp(ps->cur.text, "get") == 0 || strcmp(ps->cur.text, "set") == 0)) {
					/* peek next token: if not ':', ',', '(', '}' -> getter/setter */
					const char *q = ps->p;
					while (q < ps->end && (*q == ' ' || *q == '\t')) q++;
					if (*q != ':' && *q != ',' && *q != '(' && *q != '}') {
						prop_kind = (ps->cur.text[0] == 'g') ? 1 : 2;
						lex_next(ps);
					}
				}
				pr->kind = prop_kind;
				if (ps->cur.kind == TOK_LBRACK) {
					lex_next(ps);
					pr->key_ast = parse_assign(ps);
					expect(ps, TOK_RBRACK, "']'");
				} else if (ps->cur.kind == TOK_STR || tok_is_ident_or_kw(ps->cur.kind)) {
					pr->key_name = xstrdup_n(ps->cur.text, ps->cur.text_len);
					lex_next(ps);
				} else if (ps->cur.kind == TOK_NUM) {
					char nb[64];
					size_t nbl = fmt_js_number(ps->cur.num, nb, sizeof(nb));
					pr->key_name = xstrdup_n(nb, nbl);
					lex_next(ps);
				} else {
					parse_error(ps, "Invalid object literal property");
					return obj;
				}
				if (ps->cur.kind == TOK_LPAREN) {
					ipo_js_ast_t *fn = ast_new(ps, AST_FUNC, ps->cur.line, ps->cur.col);
					fn->u.fn.is_async = prop_async;
					parse_param_list(ps, &fn->u.fn.params, &fn->u.fn.param_len);
					fn->u.fn.body = parse_block(ps);
					pr->val = fn;
				} else if (ps->cur.kind == TOK_COLON) {
					lex_next(ps);
					pr->val = parse_assign(ps);
				} else if (pr->key_name && pr->kind == 0) {
					ipo_js_ast_t *id = ast_new(ps, AST_IDENT, ps->cur.line, ps->cur.col);
					id->u.ident.name = xstrdup_n(pr->key_name, strlen(pr->key_name));
					pr->val = id;
				} else {
					parse_error(ps, "Expected ':' in object literal");
					return obj;
				}
			}
			if (ps->cur.kind == TOK_COMMA) lex_next(ps);
			else break;
		}
		expect(ps, TOK_RBRACE, "'}'");
		return obj;
	}
	default:
		parse_error(ps, "Unexpected token in expression");
		return NULL;
	}
}

static void parse_arg_list(js_parser_t *ps, js_ast_list_t *args) {
	expect(ps, TOK_LPAREN, "'('");
	while (!ps->had_error && ps->cur.kind != TOK_RPAREN && ps->cur.kind != TOK_EOF) {
		if (ps->cur.kind == TOK_SPREAD) {
			int ln = ps->cur.line, cl = ps->cur.col;
			lex_next(ps);
			ipo_js_ast_t *sp = ast_new(ps, AST_SPREAD, ln, cl);
			sp->u.spread.arg = parse_assign(ps);
			ast_list_push(args, sp);
		} else {
			ast_list_push(args, parse_assign(ps));
		}
		if (ps->cur.kind == TOK_COMMA) lex_next(ps);
		else break;
	}
	expect(ps, TOK_RPAREN, "')'");
}

static ipo_js_ast_t *parse_postfix_call(js_parser_t *ps, int allow_call) {
	ipo_js_ast_t *expr = NULL;
	if (ps->cur.kind == TOK_NEW) {
		int ln = ps->cur.line, cl = ps->cur.col;
		lex_next(ps);
		if (ps->cur.kind == TOK_DOT) {
			/* new.target not needed as full syntax; fallback */
		}
		ipo_js_ast_t *ctor = parse_postfix_call(ps, 0);
		ipo_js_ast_t *nw = ast_new(ps, AST_NEW, ln, cl);
		nw->u.call.callee = ctor;
		if (ps->cur.kind == TOK_LPAREN) parse_arg_list(ps, &nw->u.call.args);
		expr = nw;
	} else {
		expr = parse_primary(ps);
	}

	while (!ps->had_error) {
		int ln = ps->cur.line, cl = ps->cur.col;
		if (ps->cur.kind == TOK_DOT || ps->cur.kind == TOK_OPTCHAIN) {
			int is_opt = (ps->cur.kind == TOK_OPTCHAIN);
			lex_next(ps);
			if (is_opt && ps->cur.kind == TOK_LPAREN && allow_call) {
				ipo_js_ast_t *call = ast_new(ps, AST_CALL, ln, cl);
				call->u.call.callee = expr;
				call->u.call.optional = 1;
				parse_arg_list(ps, &call->u.call.args);
				expr = call;
				continue;
			}
			if (is_opt && ps->cur.kind == TOK_LBRACK) {
				lex_next(ps);
				ipo_js_ast_t *mem = ast_new(ps, AST_MEMBER, ln, cl);
				mem->u.mem.obj = expr;
				mem->u.mem.computed = 1;
				mem->u.mem.optional = 1;
				mem->u.mem.prop_expr = parse_expr(ps);
				expect(ps, TOK_RBRACK, "']'");
				expr = mem;
				continue;
			}
			if (!tok_is_ident_or_kw(ps->cur.kind)) {
				parse_error(ps, "Expected property name after '.'");
				return expr;
			}
			ipo_js_ast_t *mem = ast_new(ps, AST_MEMBER, ln, cl);
			mem->u.mem.obj = expr;
			mem->u.mem.optional = is_opt;
			mem->u.mem.prop_name = xstrdup_n(ps->cur.text, ps->cur.text_len);
			lex_next(ps);
			expr = mem;
		} else if (ps->cur.kind == TOK_LBRACK) {
			lex_next(ps);
			ipo_js_ast_t *mem = ast_new(ps, AST_MEMBER, ln, cl);
			mem->u.mem.obj = expr;
			mem->u.mem.computed = 1;
			mem->u.mem.prop_expr = parse_expr(ps);
			expect(ps, TOK_RBRACK, "']'");
			expr = mem;
		} else if (allow_call && ps->cur.kind == TOK_LPAREN) {
			ipo_js_ast_t *call = ast_new(ps, AST_CALL, ln, cl);
			call->u.call.callee = expr;
			parse_arg_list(ps, &call->u.call.args);
			expr = call;
		} else if (allow_call && ps->cur.kind == TOK_TMPL_FULL) {
			ipo_js_ast_t *call = ast_new(ps, AST_CALL, ln, cl);
			call->u.call.callee = expr;
			ipo_js_ast_t *sarr = ast_new(ps, AST_ARRAY, ln, cl);
			ipo_js_ast_t *q0 = ast_new(ps, AST_STR, ln, cl);
			q0->u.s_lit.str = xstrdup_n(ps->cur.text, ps->cur.text_len);
			q0->u.s_lit.len = ps->cur.text_len;
			lex_next(ps);
			ast_list_push(&sarr->u.arr.elems, q0);
			ast_list_push(&call->u.call.args, sarr);
			expr = call;
		} else if (allow_call && ps->cur.kind == TOK_TMPL_HEAD) {
			ipo_js_ast_t *call = ast_new(ps, AST_CALL, ln, cl);
			call->u.call.callee = expr;
			ipo_js_ast_t *sarr = ast_new(ps, AST_ARRAY, ln, cl);
			ipo_js_ast_t *q0 = ast_new(ps, AST_STR, ln, cl);
			q0->u.s_lit.str = xstrdup_n(ps->cur.text, ps->cur.text_len);
			q0->u.s_lit.len = ps->cur.text_len;
			lex_next(ps);
			ast_list_push(&sarr->u.arr.elems, q0);
			ast_list_push(&call->u.call.args, sarr);
			while (!ps->had_error) {
				ast_list_push(&call->u.call.args, parse_expr(ps));
				if (ps->cur.kind == TOK_TMPL_MID || ps->cur.kind == TOK_TMPL_TAIL) {
					int is_tail = (ps->cur.kind == TOK_TMPL_TAIL);
					ipo_js_ast_t *qk = ast_new(ps, AST_STR, ps->cur.line, ps->cur.col);
					qk->u.s_lit.str = xstrdup_n(ps->cur.text, ps->cur.text_len);
					qk->u.s_lit.len = ps->cur.text_len;
					ast_list_push(&sarr->u.arr.elems, qk);
					lex_next(ps);
					if (is_tail) break;
				} else {
					parse_error(ps, "Expected closing template backtick");
					break;
				}
			}
			expr = call;
		} else {
			break;
		}
	}
	if (allow_call && !ps->cur.nl_before &&
	    (ps->cur.kind == TOK_INC || ps->cur.kind == TOK_DEC)) {
		ipo_js_ast_t *pf = ast_new(ps, AST_POSTFIX, ps->cur.line, ps->cur.col);
		pf->u.unary.op = ps->cur.kind;
		pf->u.unary.arg = expr;
		lex_next(ps);
		expr = pf;
	}
	return expr;
}

static ipo_js_ast_t *parse_unary_inner(js_parser_t *ps);
static ipo_js_ast_t *parse_unary(js_parser_t *ps) {
	if (ps->had_error) return NULL;
	if (ps->parse_depth >= 128) {
		parse_error(ps, "Maximum parse nesting depth exceeded");
		return NULL;
	}
	ps->parse_depth++;
	ipo_js_ast_t *r = parse_unary_inner(ps);
	ps->parse_depth--;
	return r;
}
static ipo_js_ast_t *parse_unary_inner(js_parser_t *ps) {
	if (ps->had_error) return NULL;
	int ln = ps->cur.line, cl = ps->cur.col;
	js_tok_kind_t k = ps->cur.kind;
	if (k == TOK_INC || k == TOK_DEC || k == TOK_PLUS || k == TOK_MINUS ||
	    k == TOK_NOT || k == TOK_BITNOT || k == TOK_TYPEOF || k == TOK_VOID ||
	    k == TOK_DELETE || k == TOK_AWAIT) {
		lex_next(ps);
		ipo_js_ast_t *n = ast_new(ps, AST_UNARY, ln, cl);
		n->u.unary.op = k;
		n->u.unary.arg = parse_unary(ps);
		return n;
	}
	return parse_postfix_call(ps, 1);
}

static int bin_prec(js_tok_kind_t k) {
	switch (k) {
	case TOK_POW: return 11;
	case TOK_MUL: case TOK_DIV: case TOK_MOD: return 10;
	case TOK_PLUS: case TOK_MINUS: return 9;
	case TOK_SHL: case TOK_SHR: case TOK_USHR: return 8;
	case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE:
	case TOK_IN: case TOK_INSTANCEOF: return 7;
	case TOK_EQ: case TOK_NEQ: case TOK_SEQ: case TOK_SNEQ: return 6;
	case TOK_BITAND: return 5;
	case TOK_BITXOR: return 4;
	case TOK_BITOR: return 3;
	case TOK_AND: return 2;
	case TOK_OR: case TOK_NULLISH: return 1;
	default: return 0;
	}
}

static ipo_js_ast_t *parse_binary_inner(js_parser_t *ps, int min_p);
static ipo_js_ast_t *parse_binary(js_parser_t *ps, int min_p) {
	if (ps->had_error) return NULL;
	if (ps->parse_depth >= 128) {
		parse_error(ps, "Maximum parse nesting depth exceeded");
		return NULL;
	}
	ps->parse_depth++;
	ipo_js_ast_t *r = parse_binary_inner(ps, min_p);
	ps->parse_depth--;
	return r;
}
static ipo_js_ast_t *parse_binary_inner(js_parser_t *ps, int min_p) {
	ipo_js_ast_t *left = parse_unary(ps);
	while (!ps->had_error) {
		js_tok_kind_t op = ps->cur.kind;
		int p = bin_prec(op);
		if (p < min_p) break;
		int ln = ps->cur.line, cl = ps->cur.col;
		lex_next(ps);
		int next_min = (op == TOK_POW) ? p : (p + 1);
		ipo_js_ast_t *right = parse_binary(ps, next_min);
		ipo_js_ast_t *b = ast_new(ps, AST_BINARY, ln, cl);
		b->u.bin.op = op;
		b->u.bin.left = left;
		b->u.bin.right = right;
		left = b;
	}
	return left;
}

static ipo_js_ast_t *parse_ternary(js_parser_t *ps) {
	ipo_js_ast_t *cond = parse_binary(ps, 1);
	if (!ps->had_error && ps->cur.kind == TOK_QMARK) {
		int ln = ps->cur.line, cl = ps->cur.col;
		lex_next(ps);
		ipo_js_ast_t *cons = parse_assign(ps);
		expect(ps, TOK_COLON, "':'");
		ipo_js_ast_t *alt = parse_assign(ps);
		ipo_js_ast_t *t = ast_new(ps, AST_TERNARY, ln, cl);
		t->u.tern.test = cond;
		t->u.tern.cons = cons;
		t->u.tern.alt  = alt;
		return t;
	}
	return cond;
}

static int tok_is_assign(js_tok_kind_t k) {
	return k >= TOK_ASSIGN && k <= TOK_NULLISH_ASSIGN;
}

static ipo_js_ast_t *expr_to_pattern(js_parser_t *ps, ipo_js_ast_t *e) {
	if (!e) return NULL;
	if (e->kind == AST_IDENT || e->kind == AST_MEMBER) return e;
	if (e->kind == AST_ARRAY) {
		ipo_js_ast_t *p = ast_new(ps, AST_ARR_PAT, e->line, e->col);
		size_t n = e->u.arr.elems.len;
		p->u.pat.elems = (js_pat_prop_t *)xcalloc(n ? n : 1, sizeof(js_pat_prop_t));
		p->u.pat.len = n;
		for (size_t i = 0; i < n; i++) {
			ipo_js_ast_t *item = e->u.arr.elems.items[i];
			if (item->kind == AST_UNDEF) continue;
			if (item->kind == AST_SPREAD) {
				p->u.pat.elems[i].is_rest = 1;
				p->u.pat.elems[i].target = expr_to_pattern(ps, item->u.spread.arg);
			} else if (item->kind == AST_ASSIGN && item->u.bin.op == TOK_ASSIGN) {
				p->u.pat.elems[i].target = expr_to_pattern(ps, item->u.bin.left);
				p->u.pat.elems[i].def_val = item->u.bin.right;
			} else {
				p->u.pat.elems[i].target = expr_to_pattern(ps, item);
			}
		}
		return p;
	}
	if (e->kind == AST_OBJECT) {
		ipo_js_ast_t *p = ast_new(ps, AST_OBJ_PAT, e->line, e->col);
		size_t n = e->u.obj.len;
		p->u.pat.elems = (js_pat_prop_t *)xcalloc(n ? n : 1, sizeof(js_pat_prop_t));
		p->u.pat.len = n;
		for (size_t i = 0; i < n; i++) {
			js_obj_prop_ast_t *op = &e->u.obj.props[i];
			if (op->kind == 3) {
				p->u.pat.elems[i].is_rest = 1;
				p->u.pat.elems[i].target = expr_to_pattern(ps, op->val);
			} else {
				if (op->key_name) p->u.pat.elems[i].key_name = xstrdup_n(op->key_name, strlen(op->key_name));
				p->u.pat.elems[i].key_ast = op->key_ast;
				if (op->val->kind == AST_ASSIGN && op->val->u.bin.op == TOK_ASSIGN) {
					p->u.pat.elems[i].target = expr_to_pattern(ps, op->val->u.bin.left);
					p->u.pat.elems[i].def_val = op->val->u.bin.right;
				} else {
					p->u.pat.elems[i].target = expr_to_pattern(ps, op->val);
				}
			}
		}
		return p;
	}
	return e;
}

static ipo_js_ast_t *parse_assign_inner(js_parser_t *ps);
static ipo_js_ast_t *parse_assign(js_parser_t *ps) {
	if (ps->had_error) return NULL;
	if (ps->parse_depth >= 128) {
		parse_error(ps, "Maximum parse nesting depth exceeded");
		return NULL;
	}
	ps->parse_depth++;
	ipo_js_ast_t *r = parse_assign_inner(ps);
	ps->parse_depth--;
	return r;
}
static ipo_js_ast_t *parse_assign_inner(js_parser_t *ps) {
	if (try_is_arrow_params(ps)) {
		return parse_arrow_fn(ps, 0);
	}
	ipo_js_ast_t *left = parse_ternary(ps);
	if (!ps->had_error && tok_is_assign(ps->cur.kind)) {
		int ln = ps->cur.line, cl = ps->cur.col;
		js_tok_kind_t op = ps->cur.kind;
		lex_next(ps);
		ipo_js_ast_t *right = parse_assign(ps);
		ipo_js_ast_t *as = ast_new(ps, AST_ASSIGN, ln, cl);
		as->u.bin.op = op;
		as->u.bin.left = (op == TOK_ASSIGN && (left->kind == AST_ARRAY || left->kind == AST_OBJECT))
		                     ? expr_to_pattern(ps, left) : left;
		as->u.bin.right = right;
		return as;
	}
	return left;
}

static ipo_js_ast_t *parse_expr(js_parser_t *ps) {
	ipo_js_ast_t *first = parse_assign(ps);
	if (!ps->had_error && ps->cur.kind == TOK_COMMA) {
		ipo_js_ast_t *seq = ast_new(ps, AST_SEQ, first->line, first->col);
		ast_list_push(&seq->u.seq.exprs, first);
		while (!ps->had_error && ps->cur.kind == TOK_COMMA) {
			lex_next(ps);
			ast_list_push(&seq->u.seq.exprs, parse_assign(ps));
		}
		return seq;
	}
	return first;
}

static ipo_js_ast_t *parse_block(js_parser_t *ps) {
	int ln = ps->cur.line, cl = ps->cur.col;
	expect(ps, TOK_LBRACE, "'{'");
	ipo_js_ast_t *blk = ast_new(ps, AST_BLOCK, ln, cl);
	while (!ps->had_error && ps->cur.kind != TOK_RBRACE && ps->cur.kind != TOK_EOF) {
		ast_list_push(&blk->u.block.stmts, parse_stmt(ps));
	}
	expect(ps, TOK_RBRACE, "'}'");
	return blk;
}

static ipo_js_ast_t *parse_function(js_parser_t *ps, int is_decl, int is_async) {
	int ln = ps->cur.line, cl = ps->cur.col;
	expect(ps, TOK_FUNCTION, "'function'");
	/* ignore generator '*' if any */
	if (ps->cur.kind == TOK_MUL) lex_next(ps);
	ipo_js_ast_t *fn = ast_new(ps, AST_FUNC, ln, cl);
	fn->u.fn.is_decl = is_decl;
	fn->u.fn.is_async = is_async;
	if (ps->cur.kind == TOK_IDENT || ps->cur.kind == TOK_ASYNC || ps->cur.kind == TOK_OF) {
		fn->u.fn.name = xstrdup_n(ps->cur.text, ps->cur.text_len);
		lex_next(ps);
	} else if (is_decl) {
		parse_error(ps, "Function declaration requires a name");
	}
	parse_param_list(ps, &fn->u.fn.params, &fn->u.fn.param_len);
	fn->u.fn.body = parse_block(ps);
	return fn;
}

static ipo_js_ast_t *parse_class(js_parser_t *ps, int is_decl) {
	int ln = ps->cur.line, cl = ps->cur.col;
	expect(ps, TOK_CLASS, "'class'");
	ipo_js_ast_t *cls = ast_new(ps, AST_CLASS, ln, cl);
	cls->u.cls.is_decl = is_decl;
	if (ps->cur.kind == TOK_IDENT) {
		cls->u.cls.name = xstrdup_n(ps->cur.text, ps->cur.text_len);
		lex_next(ps);
	} else if (is_decl) {
		parse_error(ps, "Class declaration requires a name");
	}
	if (ps->cur.kind == TOK_EXTENDS) {
		lex_next(ps);
		cls->u.cls.super_expr = parse_postfix_call(ps, 0);
	}
	expect(ps, TOK_LBRACE, "'{'");
	size_t cap = 0;
	while (!ps->had_error && ps->cur.kind != TOK_RBRACE && ps->cur.kind != TOK_EOF) {
		if (ps->cur.kind == TOK_SEMI) { lex_next(ps); continue; }
		if (cls->u.cls.elem_len >= cap) {
			cap = cap ? cap * 2 : 4;
			cls->u.cls.elems = (js_class_elem_t *)xrealloc(cls->u.cls.elems, cap * sizeof(js_class_elem_t));
		}
		js_class_elem_t *el = &cls->u.cls.elems[cls->u.cls.elem_len++];
		memset(el, 0, sizeof(*el));
		if (ps->cur.kind == TOK_IDENT && strcmp(ps->cur.text, "static") == 0) {
			const char *q = ps->p;
			while (q < ps->end && (*q == ' ' || *q == '\t')) q++;
			if (*q != '(' && *q != '=') {
				el->is_static = 1;
				lex_next(ps);
			}
		}
		int is_async_m = 0;
		if (ps->cur.kind == TOK_ASYNC) {
			const char *q = ps->p;
			while (q < ps->end && (*q == ' ' || *q == '\t')) q++;
			if (*q != '(') {
				is_async_m = 1;
				lex_next(ps);
			}
		}
		if (ps->cur.kind == TOK_IDENT &&
		    (strcmp(ps->cur.text, "get") == 0 || strcmp(ps->cur.text, "set") == 0)) {
			const char *q = ps->p;
			while (q < ps->end && (*q == ' ' || *q == '\t')) q++;
			if (*q != '(') {
				el->kind = (ps->cur.text[0] == 'g') ? 1 : 2;
				lex_next(ps);
			}
		}
		if (ps->cur.kind == TOK_LBRACK) {
			lex_next(ps);
			el->key_ast = parse_assign(ps);
			expect(ps, TOK_RBRACK, "']'");
		} else if (tok_is_ident_or_kw(ps->cur.kind) || ps->cur.kind == TOK_STR) {
			el->name = xstrdup_n(ps->cur.text, ps->cur.text_len);
			lex_next(ps);
		} else {
			parse_error(ps, "Expected class member name");
			break;
		}
		ipo_js_ast_t *fn = ast_new(ps, AST_FUNC, ps->cur.line, ps->cur.col);
		fn->u.fn.is_async = is_async_m;
		parse_param_list(ps, &fn->u.fn.params, &fn->u.fn.param_len);
		fn->u.fn.body = parse_block(ps);
		el->fn_ast = fn;
	}
	expect(ps, TOK_RBRACE, "'}'");
	return cls;
}

static ipo_js_ast_t *parse_var_decl(js_parser_t *ps, int decl_kind, int for_head) {
	int ln = ps->cur.line, cl = ps->cur.col;
	ipo_js_ast_t *vd = ast_new(ps, AST_VAR_DECL, ln, cl);
	vd->u.var_decl.decl_kind = decl_kind;
	size_t cap = 0;
	for (;;) {
		if (vd->u.var_decl.len >= cap) {
			cap = cap ? cap * 2 : 2;
			vd->u.var_decl.targets = (ipo_js_ast_t **)xrealloc(vd->u.var_decl.targets, cap * sizeof(ipo_js_ast_t *));
			vd->u.var_decl.inits   = (ipo_js_ast_t **)xrealloc(vd->u.var_decl.inits, cap * sizeof(ipo_js_ast_t *));
		}
		ipo_js_ast_t *pat = parse_pattern(ps);
		ipo_js_ast_t *init = NULL;
		if (!ps->had_error && ps->cur.kind == TOK_ASSIGN) {
			lex_next(ps);
			init = parse_assign(ps);
		}
		vd->u.var_decl.targets[vd->u.var_decl.len] = pat;
		vd->u.var_decl.inits[vd->u.var_decl.len] = init;
		vd->u.var_decl.len++;
		if (!for_head && ps->cur.kind == TOK_COMMA) lex_next(ps);
		else if (for_head && ps->cur.kind == TOK_COMMA) lex_next(ps);
		else break;
	}
	if (!for_head) consume_semi(ps);
	return vd;
}

static ipo_js_ast_t *parse_stmt_inner(js_parser_t *ps);
static ipo_js_ast_t *parse_stmt(js_parser_t *ps) {
	if (ps->had_error) return NULL;
	if (ps->parse_depth >= 128) {
		parse_error(ps, "Maximum parse nesting depth exceeded");
		return NULL;
	}
	ps->parse_depth++;
	ipo_js_ast_t *r = parse_stmt_inner(ps);
	ps->parse_depth--;
	return r;
}
static ipo_js_ast_t *parse_stmt_inner(js_parser_t *ps) {
	if (ps->had_error) return NULL;
	int ln = ps->cur.line, cl = ps->cur.col;

	if (ps->cur.kind == TOK_SEMI) {
		lex_next(ps);
		return ast_new(ps, AST_EMPTY, ln, cl);
	}
	if (ps->cur.kind == TOK_LBRACE) {
		return parse_block(ps);
	}
	if (ps->cur.kind == TOK_VAR || ps->cur.kind == TOK_LET || ps->cur.kind == TOK_CONST) {
		int dk = (ps->cur.kind == TOK_VAR) ? 0 : (ps->cur.kind == TOK_LET ? 1 : 2);
		lex_next(ps);
		return parse_var_decl(ps, dk, 0);
	}
	if (ps->cur.kind == TOK_FUNCTION) {
		return parse_function(ps, 1, 0);
	}
	if (ps->cur.kind == TOK_ASYNC) {
		const char *q = ps->p;
		while (q < ps->end && (*q == ' ' || *q == '\t')) q++;
		if (strncmp(q, "function", 8) == 0) {
			lex_next(ps);
			return parse_function(ps, 1, 1);
		}
	}
	if (ps->cur.kind == TOK_CLASS) {
		return parse_class(ps, 1);
	}
	if (ps->cur.kind == TOK_IF) {
		lex_next(ps);
		ipo_js_ast_t *n = ast_new(ps, AST_IF, ln, cl);
		expect(ps, TOK_LPAREN, "'('");
		n->u.if_s.test = parse_expr(ps);
		expect(ps, TOK_RPAREN, "')'");
		n->u.if_s.cons = parse_stmt(ps);
		if (ps->cur.kind == TOK_ELSE) {
			lex_next(ps);
			n->u.if_s.alt = parse_stmt(ps);
		}
		return n;
	}
	if (ps->cur.kind == TOK_WHILE) {
		lex_next(ps);
		ipo_js_ast_t *n = ast_new(ps, AST_WHILE, ln, cl);
		expect(ps, TOK_LPAREN, "'('");
		n->u.loop_s.test = parse_expr(ps);
		expect(ps, TOK_RPAREN, "')'");
		n->u.loop_s.body = parse_stmt(ps);
		return n;
	}
	if (ps->cur.kind == TOK_DO) {
		lex_next(ps);
		ipo_js_ast_t *n = ast_new(ps, AST_DO_WHILE, ln, cl);
		n->u.loop_s.body = parse_stmt(ps);
		expect(ps, TOK_WHILE, "'while'");
		expect(ps, TOK_LPAREN, "'('");
		n->u.loop_s.test = parse_expr(ps);
		expect(ps, TOK_RPAREN, "')'");
		if (ps->cur.kind == TOK_SEMI) lex_next(ps);
		return n;
	}
	if (ps->cur.kind == TOK_FOR) {
		lex_next(ps);
		expect(ps, TOK_LPAREN, "'('");
		ipo_js_ast_t *init = NULL;
		int dk = -1;
		if (ps->cur.kind != TOK_SEMI) {
			if (ps->cur.kind == TOK_VAR || ps->cur.kind == TOK_LET || ps->cur.kind == TOK_CONST) {
				dk = (ps->cur.kind == TOK_VAR) ? 0 : (ps->cur.kind == TOK_LET ? 1 : 2);
				lex_next(ps);
				ipo_js_ast_t *lhs = parse_pattern(ps);
				if (ps->cur.kind == TOK_IN || ps->cur.kind == TOK_OF) {
					js_ast_kind_t fk = (ps->cur.kind == TOK_IN) ? AST_FOR_IN : AST_FOR_OF;
					lex_next(ps);
					ipo_js_ast_t *rhs = parse_expr(ps);
					expect(ps, TOK_RPAREN, "')'");
					ipo_js_ast_t *n = ast_new(ps, fk, ln, cl);
					n->u.for_iter.decl_kind = dk;
					n->u.for_iter.lhs = lhs;
					n->u.for_iter.rhs = rhs;
					n->u.for_iter.body = parse_stmt(ps);
					return n;
				}
				/* Regular for (var a = 0, b = 1; ...) */
				ipo_js_ast_t *vd = ast_new(ps, AST_VAR_DECL, lhs->line, lhs->col);
				vd->u.var_decl.decl_kind = dk;
				size_t cap = 2;
				vd->u.var_decl.targets = (ipo_js_ast_t **)xmalloc(cap * sizeof(ipo_js_ast_t *));
				vd->u.var_decl.inits   = (ipo_js_ast_t **)xmalloc(cap * sizeof(ipo_js_ast_t *));
				vd->u.var_decl.targets[0] = lhs;
				vd->u.var_decl.inits[0]   = (ps->cur.kind == TOK_ASSIGN) ? (lex_next(ps), parse_assign(ps)) : NULL;
				vd->u.var_decl.len = 1;
				while (ps->cur.kind == TOK_COMMA) {
					lex_next(ps);
					if (vd->u.var_decl.len >= cap) {
						cap *= 2;
						vd->u.var_decl.targets = (ipo_js_ast_t **)xrealloc(vd->u.var_decl.targets, cap * sizeof(ipo_js_ast_t *));
						vd->u.var_decl.inits   = (ipo_js_ast_t **)xrealloc(vd->u.var_decl.inits, cap * sizeof(ipo_js_ast_t *));
					}
					vd->u.var_decl.targets[vd->u.var_decl.len] = parse_pattern(ps);
					vd->u.var_decl.inits[vd->u.var_decl.len]   = (ps->cur.kind == TOK_ASSIGN) ? (lex_next(ps), parse_assign(ps)) : NULL;
					vd->u.var_decl.len++;
				}
				init = vd;
			} else {
				/* Wait: parse_expr handles `for (k in obj)`? Because TOK_IN is binary precedence! */
				ipo_js_ast_t *e = parse_expr(ps);
				if (e && e->kind == AST_BINARY && e->u.bin.op == TOK_IN) {
					expect(ps, TOK_RPAREN, "')'");
					ipo_js_ast_t *n = ast_new(ps, AST_FOR_IN, ln, cl);
					n->u.for_iter.decl_kind = -1;
					n->u.for_iter.lhs = expr_to_pattern(ps, e->u.bin.left);
					n->u.for_iter.rhs = e->u.bin.right;
					n->u.for_iter.body = parse_stmt(ps);
					return n;
				}
				if (ps->cur.kind == TOK_OF) {
					lex_next(ps);
					ipo_js_ast_t *rhs = parse_expr(ps);
					expect(ps, TOK_RPAREN, "')'");
					ipo_js_ast_t *n = ast_new(ps, AST_FOR_OF, ln, cl);
					n->u.for_iter.decl_kind = -1;
					n->u.for_iter.lhs = expr_to_pattern(ps, e);
					n->u.for_iter.rhs = rhs;
					n->u.for_iter.body = parse_stmt(ps);
					return n;
				}
				init = e;
			}
		}
		expect(ps, TOK_SEMI, "';'");
		ipo_js_ast_t *test = (ps->cur.kind != TOK_SEMI) ? parse_expr(ps) : NULL;
		expect(ps, TOK_SEMI, "';'");
		ipo_js_ast_t *upd = (ps->cur.kind != TOK_RPAREN) ? parse_expr(ps) : NULL;
		expect(ps, TOK_RPAREN, "')'");
		ipo_js_ast_t *n = ast_new(ps, AST_FOR, ln, cl);
		n->u.for_s.init = init;
		n->u.for_s.test = test;
		n->u.for_s.upd  = upd;
		n->u.for_s.body = parse_stmt(ps);
		return n;
	}
	if (ps->cur.kind == TOK_SWITCH) {
		lex_next(ps);
		ipo_js_ast_t *n = ast_new(ps, AST_SWITCH, ln, cl);
		expect(ps, TOK_LPAREN, "'('");
		n->u.sw.disc = parse_expr(ps);
		expect(ps, TOK_RPAREN, "')'");
		expect(ps, TOK_LBRACE, "'{'");
		size_t cap = 0;
		while (!ps->had_error && ps->cur.kind != TOK_RBRACE && ps->cur.kind != TOK_EOF) {
			if (n->u.sw.len >= cap) {
				cap = cap ? cap * 2 : 4;
				n->u.sw.cases = (js_case_clause_t *)xrealloc(n->u.sw.cases, cap * sizeof(js_case_clause_t));
			}
			js_case_clause_t *cc = &n->u.sw.cases[n->u.sw.len++];
			memset(cc, 0, sizeof(*cc));
			if (ps->cur.kind == TOK_CASE) {
				lex_next(ps);
				cc->test = parse_expr(ps);
				expect(ps, TOK_COLON, "':'");
			} else if (ps->cur.kind == TOK_DEFAULT) {
				lex_next(ps);
				expect(ps, TOK_COLON, "':'");
			} else {
				parse_error(ps, "Expected 'case' or 'default'");
				break;
			}
			while (!ps->had_error && ps->cur.kind != TOK_CASE && ps->cur.kind != TOK_DEFAULT &&
			       ps->cur.kind != TOK_RBRACE && ps->cur.kind != TOK_EOF) {
				ast_list_push(&cc->body, parse_stmt(ps));
			}
		}
		expect(ps, TOK_RBRACE, "'}'");
		return n;
	}
	if (ps->cur.kind == TOK_BREAK || ps->cur.kind == TOK_CONTINUE) {
		js_ast_kind_t jk = (ps->cur.kind == TOK_BREAK) ? AST_BREAK : AST_CONTINUE;
		lex_next(ps);
		ipo_js_ast_t *n = ast_new(ps, jk, ln, cl);
		if (!ps->cur.nl_before && ps->cur.kind == TOK_IDENT) {
			n->u.jump.label = xstrdup_n(ps->cur.text, ps->cur.text_len);
			lex_next(ps);
		}
		consume_semi(ps);
		return n;
	}
	if (ps->cur.kind == TOK_RETURN) {
		lex_next(ps);
		ipo_js_ast_t *n = ast_new(ps, AST_RETURN, ln, cl);
		if (ps->cur.kind != TOK_SEMI && ps->cur.kind != TOK_RBRACE &&
		    ps->cur.kind != TOK_EOF && !ps->cur.nl_before) {
			n->u.expr_stmt.expr = parse_expr(ps);
		}
		consume_semi(ps);
		return n;
	}
	if (ps->cur.kind == TOK_THROW) {
		lex_next(ps);
		ipo_js_ast_t *n = ast_new(ps, AST_THROW, ln, cl);
		n->u.expr_stmt.expr = parse_expr(ps);
		consume_semi(ps);
		return n;
	}
	if (ps->cur.kind == TOK_TRY) {
		lex_next(ps);
		ipo_js_ast_t *n = ast_new(ps, AST_TRY, ln, cl);
		n->u.try_s.block = parse_block(ps);
		if (ps->cur.kind == TOK_CATCH) {
			lex_next(ps);
			if (ps->cur.kind == TOK_LPAREN) {
				lex_next(ps);
				n->u.try_s.catch_pat = parse_pattern(ps);
				expect(ps, TOK_RPAREN, "')'");
			}
			n->u.try_s.catch_block = parse_block(ps);
		}
		if (ps->cur.kind == TOK_FINALLY) {
			lex_next(ps);
			n->u.try_s.finally_block = parse_block(ps);
		}
		return n;
	}

	/* Label or expression statement */
	if (ps->cur.kind == TOK_IDENT) {
		const char *q = ps->p;
		while (q < ps->end && (*q == ' ' || *q == '\t')) q++;
		if (*q == ':') {
			char *lbl = xstrdup_n(ps->cur.text, ps->cur.text_len);
			lex_next(ps);
			expect(ps, TOK_COLON, "':'");
			ipo_js_ast_t *n = ast_new(ps, AST_LABEL, ln, cl);
			n->u.labeled.label = lbl;
			n->u.labeled.stmt = parse_stmt(ps);
			return n;
		}
	}

	ipo_js_ast_t *ex = parse_expr(ps);
	consume_semi(ps);
	ipo_js_ast_t *st = ast_new(ps, AST_EXPR_STMT, ln, cl);
	st->u.expr_stmt.expr = ex;
	return st;
}

static js_ast_pool_t *parse_program(ipo_js_vm_t *vm, const char *code, size_t len,
                                    const char *filename, ipo_js_ast_t **out_root) {
	js_parser_t ps;
	memset(&ps, 0, sizeof(ps));
	ps.src = code ? code : "";
	ps.end = ps.src + len;
	ps.p = ps.src;
	ps.line = 1;
	ps.line_start = ps.src;
	ps.filename = filename;
	ps.prev_kind = TOK_EOF;
	ps.pool = (js_ast_pool_t *)xcalloc(1, sizeof(js_ast_pool_t));

	lex_next(&ps);
	ipo_js_ast_t *prog = ast_new(&ps, AST_BLOCK, 1, 1);
	while (!ps.had_error && ps.cur.kind != TOK_EOF) {
		ast_list_push(&prog->u.block.stmts, parse_stmt(&ps));
	}
	free(ps.cur.text);
	free(ps.cur.extra);

	if (ps.had_error) {
		ipo_js_throw_msg(vm, "SyntaxError", "%s", ps.err_msg);
		free_ast_pool(ps.pool);
		*out_root = NULL;
		return NULL;
	}
	*out_root = prog;
	return ps.pool;
}

/* =========================================================================
 * Environment Scoping & Hoisting
 * ========================================================================= */

static js_binding_t *env_find_own(ipo_js_env_t *e, const char *name) {
	uint32_t h = fnv1a_hash(name, strlen(name));
	for (size_t i = 0; i < e->len; i++) {
		if (e->vars[i].name && e->vars[i].name->hash == h &&
		    strcmp(e->vars[i].name->data, name) == 0) {
			return &e->vars[i];
		}
	}
	return NULL;
}

static js_binding_t *env_def(ipo_js_vm_t *vm, ipo_js_env_t *e, const char *name,
                             ipo_js_val_t val, int is_const) {
	if (e->with_obj) {
		ipo_js_set_prop(vm, e->with_obj, name, val);
		return NULL;
	}
	js_binding_t *b = env_find_own(e, name);
	if (!b) {
		if (e->len >= e->cap) {
			size_t ncap = e->cap ? e->cap * 2 : 8;
			e->vars = (js_binding_t *)xrealloc(e->vars, ncap * sizeof(js_binding_t));
			e->cap = ncap;
		}
		b = &e->vars[e->len++];
		b->name = vm_alloc_str(vm, name, strlen(name));
	}
	b->val = val;
	b->is_const = (uint8_t)is_const;
	return b;
}

static int env_get(ipo_js_vm_t *vm, ipo_js_env_t *e, const char *name, ipo_js_val_t *out) {
	for (ipo_js_env_t *cur = e; cur; cur = cur->parent) {
		if (cur->with_obj) {
			if (ipo_js_get_prop(vm, cur->with_obj, name, out)) return 1;
		} else {
			js_binding_t *b = env_find_own(cur, name);
			if (b) {
				*out = b->val;
				return 1;
			}
		}
	}
	*out = ipo_js_undef();
	return 0;
}

static void env_set(ipo_js_vm_t *vm, ipo_js_env_t *e, const char *name, ipo_js_val_t val) {
	for (ipo_js_env_t *cur = e; cur; cur = cur->parent) {
		if (cur->with_obj) {
			if (!cur->parent || ipo_js_has_own_prop(cur->with_obj, name)) {
				ipo_js_set_prop(vm, cur->with_obj, name, val);
				return;
			}
		} else {
			js_binding_t *b = env_find_own(cur, name);
			if (b) {
				if (b->is_const) {
					ipo_js_throw_msg(vm, "TypeError", "Assignment to constant variable '%s'", name);
					return;
				}
				b->val = val;
				return;
			}
		}
	}
	/* Implicit global assignment */
	ipo_js_set_prop(vm, vm->global_obj, name, val);
}

static ipo_js_env_t *env_var_scope(ipo_js_env_t *e) {
	while (e && e->parent && !e->is_func_scope) e = e->parent;
	return e;
}

static ipo_js_obj_t *vm_new_js_fn(ipo_js_vm_t *vm, ipo_js_ast_t *ast, ipo_js_env_t *closure) {
	ipo_js_obj_t *fn = vm_alloc_obj(vm, JS_OBJ_FUNCTION, vm->fn_proto);
	if (!fn) return NULL;
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(fn));
	fn->u.fn.func_ast = ast;
	fn->u.fn.closure = closure;
	fn->u.fn.is_arrow = ast->u.fn.is_arrow;
	fn->u.fn.name = ast->u.fn.name ? vm_alloc_str(vm, ast->u.fn.name, strlen(ast->u.fn.name))
	                               : vm_alloc_str(vm, "", 0);
	if (!ast->u.fn.is_arrow) {
		ipo_js_obj_t *proto = ipo_js_new_object(vm, vm->obj_proto);
		ipo_js_set_prop(vm, fn, "prototype", ipo_js_obj_val(proto));
		ipo_js_set_prop(vm, proto, "constructor", ipo_js_obj_val(fn));
	}
	ipo_js_gc_pop_roots(vm, top);
	return fn;
}

static void hoist_pattern_vars(ipo_js_vm_t *vm, ipo_js_env_t *var_env, ipo_js_ast_t *pat) {
	if (!pat) return;
	if (pat->kind == AST_IDENT) {
		ipo_js_val_t dummy;
		if (var_env->with_obj) {
			if (!ipo_js_has_own_prop(var_env->with_obj, pat->u.ident.name))
				ipo_js_set_prop(vm, var_env->with_obj, pat->u.ident.name, ipo_js_undef());
		} else if (!env_find_own(var_env, pat->u.ident.name)) {
			env_def(vm, var_env, pat->u.ident.name, ipo_js_undef(), 0);
		}
		(void)dummy;
	} else if (pat->kind == AST_ARR_PAT || pat->kind == AST_OBJ_PAT) {
		for (size_t i = 0; i < pat->u.pat.len; i++) {
			hoist_pattern_vars(vm, var_env, pat->u.pat.elems[i].target);
		}
	}
}

static void hoist_block(ipo_js_vm_t *vm, ipo_js_env_t *var_env, ipo_js_env_t *lex_env,
                        ipo_js_ast_t *node, int recurse_blocks) {
	if (!node || vm->ast_depth >= 256) return;
	vm->ast_depth++;
	switch (node->kind) {
	case AST_BLOCK:
		for (size_t i = 0; i < node->u.block.stmts.len; i++) {
			ipo_js_ast_t *st = node->u.block.stmts.items[i];
			if (st->kind == AST_FUNC && st->u.fn.is_decl && st->u.fn.name) {
				ipo_js_obj_t *fobj = vm_new_js_fn(vm, st, lex_env);
				env_def(vm, lex_env, st->u.fn.name, ipo_js_obj_val(fobj), 0);
			} else if (st->kind == AST_VAR_DECL && st->u.var_decl.decl_kind == 0) {
				for (size_t j = 0; j < st->u.var_decl.len; j++)
					hoist_pattern_vars(vm, var_env, st->u.var_decl.targets[j]);
			} else if (recurse_blocks) {
				hoist_block(vm, var_env, lex_env, st, 1);
			}
		}
		break;
	case AST_IF:
		hoist_block(vm, var_env, lex_env, node->u.if_s.cons, 1);
		hoist_block(vm, var_env, lex_env, node->u.if_s.alt, 1);
		break;
	case AST_WHILE:
	case AST_DO_WHILE:
		hoist_block(vm, var_env, lex_env, node->u.loop_s.body, 1);
		break;
	case AST_FOR:
		if (node->u.for_s.init && node->u.for_s.init->kind == AST_VAR_DECL &&
		    node->u.for_s.init->u.var_decl.decl_kind == 0) {
			for (size_t j = 0; j < node->u.for_s.init->u.var_decl.len; j++)
				hoist_pattern_vars(vm, var_env, node->u.for_s.init->u.var_decl.targets[j]);
		}
		hoist_block(vm, var_env, lex_env, node->u.for_s.body, 1);
		break;
	case AST_FOR_IN:
	case AST_FOR_OF:
		if (node->u.for_iter.decl_kind == 0) hoist_pattern_vars(vm, var_env, node->u.for_iter.lhs);
		hoist_block(vm, var_env, lex_env, node->u.for_iter.body, 1);
		break;
	case AST_TRY:
		hoist_block(vm, var_env, lex_env, node->u.try_s.block, 1);
		hoist_block(vm, var_env, lex_env, node->u.try_s.catch_block, 1);
		hoist_block(vm, var_env, lex_env, node->u.try_s.finally_block, 1);
		break;
	case AST_SWITCH:
		for (size_t i = 0; i < node->u.sw.len; i++) {
			for (size_t j = 0; j < node->u.sw.cases[i].body.len; j++) {
				hoist_block(vm, var_env, lex_env, node->u.sw.cases[i].body.items[j], 1);
			}
		}
		break;
	default:
		break;
	}
	vm->ast_depth--;
}

/* =========================================================================
 * LValue & Destructuring Assignment
 * ========================================================================= */

static void bind_or_assign_pattern(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *pat,
                                   ipo_js_val_t val, int decl_kind /* -1=assign, 0=var, 1=let, 2=const */) {
	if (!pat || vm->sig != SIG_NONE) return;
	size_t top = ipo_js_gc_push_root(vm, val);

	if (pat->kind == AST_IDENT) {
		if (decl_kind < 0) env_set(vm, env, pat->u.ident.name, val);
		else if (decl_kind == 0) env_def(vm, env_var_scope(env), pat->u.ident.name, val, 0);
		else env_def(vm, env, pat->u.ident.name, val, decl_kind == 2);
		ipo_js_gc_pop_roots(vm, top);
		return;
	}
	if (pat->kind == AST_MEMBER && decl_kind < 0) {
		ipo_js_val_t obj = vm_eval_expr(vm, env, pat->u.mem.obj);
		if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return; }
		if (pat->u.mem.computed) {
			ipo_js_val_t kv = vm_eval_expr(vm, env, pat->u.mem.prop_expr);
			if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return; }
			const char *k = ipo_js_cstr(vm, kv, NULL);
			vm_set_prop_val(vm, obj, k, val);
		} else {
			vm_set_prop_val(vm, obj, pat->u.mem.prop_name, val);
		}
		ipo_js_gc_pop_roots(vm, top);
		return;
	}
	if (pat->kind == AST_ARR_PAT) {
		size_t alen = (val.type == JS_T_OBJECT && val.u.obj && val.u.obj->kind == JS_OBJ_ARRAY)
		                  ? val.u.obj->u.arr.len
		                  : (val.type == JS_T_STRING && val.u.str ? val.u.str->len : 0);
		for (size_t i = 0; i < pat->u.pat.len; i++) {
			js_pat_prop_t *el = &pat->u.pat.elems[i];
			if (!el->target) continue;
			if (el->is_rest) {
				ipo_js_obj_t *rest = ipo_js_new_array(vm, 0);
				size_t rtop = ipo_js_gc_push_root(vm, ipo_js_obj_val(rest));
				for (size_t j = i; j < alen; j++) {
					char nb[24];
					snprintf(nb, sizeof(nb), "%lu", (unsigned long)j);
					ipo_js_val_t iv = ipo_js_undef();
					vm_get_prop_val(vm, val, nb, &iv);
					ipo_js_arr_push(vm, rest, iv);
				}
				bind_or_assign_pattern(vm, env, el->target, ipo_js_obj_val(rest), decl_kind);
				ipo_js_gc_pop_roots(vm, rtop);
				break;
			}
			char nb[24];
			snprintf(nb, sizeof(nb), "%lu", (unsigned long)i);
			ipo_js_val_t iv = ipo_js_undef();
			if (i < alen) vm_get_prop_val(vm, val, nb, &iv);
			if (iv.type == JS_T_UNDEFINED && el->def_val) {
				iv = vm_eval_expr(vm, env, el->def_val);
				if (vm->sig != SIG_NONE) break;
			}
			bind_or_assign_pattern(vm, env, el->target, iv, decl_kind);
		}
		ipo_js_gc_pop_roots(vm, top);
		return;
	}
	if (pat->kind == AST_OBJ_PAT) {
		if (val.type != JS_T_OBJECT && val.type != JS_T_STRING) {
			ipo_js_throw_msg(vm, "TypeError", "Cannot destructure non-object");
			ipo_js_gc_pop_roots(vm, top);
			return;
		}
		for (size_t i = 0; i < pat->u.pat.len; i++) {
			js_pat_prop_t *el = &pat->u.pat.elems[i];
			if (el->is_rest) {
				ipo_js_obj_t *rest = ipo_js_new_object(vm, NULL);
				if (val.type == JS_T_OBJECT && val.u.obj) {
					for (size_t j = 0; j < val.u.obj->prop_len; j++) {
						const char *k = val.u.obj->props[j].key->data;
						int excluded = 0;
						for (size_t m = 0; m < i; m++) {
							if (pat->u.pat.elems[m].key_name && strcmp(pat->u.pat.elems[m].key_name, k) == 0) {
								excluded = 1; break;
							}
						}
						if (!excluded) ipo_js_set_prop(vm, rest, k, val.u.obj->props[j].val);
					}
				}
				bind_or_assign_pattern(vm, env, el->target, ipo_js_obj_val(rest), decl_kind);
				continue;
			}
			const char *kname = el->key_name;
			if (!kname && el->key_ast) {
				ipo_js_val_t kv = vm_eval_expr(vm, env, el->key_ast);
				if (vm->sig != SIG_NONE) break;
				kname = ipo_js_cstr(vm, kv, NULL);
			}
			ipo_js_val_t pv = ipo_js_undef();
			if (kname) vm_get_prop_val(vm, val, kname, &pv);
			if (pv.type == JS_T_UNDEFINED && el->def_val) {
				pv = vm_eval_expr(vm, env, el->def_val);
				if (vm->sig != SIG_NONE) break;
			}
			bind_or_assign_pattern(vm, env, el->target, pv, decl_kind);
		}
		ipo_js_gc_pop_roots(vm, top);
		return;
	}
	ipo_js_throw_msg(vm, "SyntaxError", "Invalid left-hand side in assignment");
	ipo_js_gc_pop_roots(vm, top);
}

static ipo_js_val_t read_lvalue(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *lhs) {
	if (lhs->kind == AST_IDENT) {
		ipo_js_val_t v;
		if (!env_get(vm, env, lhs->u.ident.name, &v)) {
			ipo_js_throw_msg(vm, "ReferenceError", "%s is not defined", lhs->u.ident.name);
		}
		return v;
	}
	if (lhs->kind == AST_MEMBER) {
		ipo_js_val_t obj = vm_eval_expr(vm, env, lhs->u.mem.obj);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		const char *k = lhs->u.mem.prop_name;
		if (lhs->u.mem.computed) {
			ipo_js_val_t kv = vm_eval_expr(vm, env, lhs->u.mem.prop_expr);
			if (vm->sig != SIG_NONE) return ipo_js_undef();
			k = ipo_js_cstr(vm, kv, NULL);
		}
		ipo_js_val_t out = ipo_js_undef();
		vm_get_prop_val(vm, obj, k, &out);
		return out;
	}
	ipo_js_throw_msg(vm, "ReferenceError", "Invalid lvalue");
	return ipo_js_undef();
}

/* =========================================================================
 * Evaluator Core
 * ========================================================================= */

static ipo_js_val_t apply_binop(ipo_js_vm_t *vm, int op, ipo_js_val_t l, ipo_js_val_t r) {
	switch (op) {
	case TOK_PLUS:
		if (l.type == JS_T_OBJECT) l = to_primitive(vm, l, 0);
		if (r.type == JS_T_OBJECT) r = to_primitive(vm, r, 0);
		if (l.type == JS_T_STRING || r.type == JS_T_STRING) {
			ipo_js_str_t *ls = ipo_js_to_str(vm, l);
			size_t top = ipo_js_gc_push_root(vm, (ipo_js_val_t){.type = JS_T_STRING, .u.str = ls});
			ipo_js_str_t *rs = ipo_js_to_str(vm, r);
			js_buf_t b;
			jbuf_init(&b);
			jbuf_append(&b, ls->data, ls->len);
			jbuf_append(&b, rs->data, rs->len);
			ipo_js_val_t out = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
			jbuf_free(&b);
			ipo_js_gc_pop_roots(vm, top);
			return out;
		}
		return ipo_js_num(ipo_js_to_num(vm, l) + ipo_js_to_num(vm, r));
	case TOK_MINUS: return ipo_js_num(ipo_js_to_num(vm, l) - ipo_js_to_num(vm, r));
	case TOK_MUL:   return ipo_js_num(ipo_js_to_num(vm, l) * ipo_js_to_num(vm, r));
	case TOK_DIV:   return ipo_js_num(ipo_js_to_num(vm, l) / ipo_js_to_num(vm, r));
	case TOK_MOD:   return ipo_js_num(fmod(ipo_js_to_num(vm, l), ipo_js_to_num(vm, r)));
	case TOK_POW:   return ipo_js_num(pow(ipo_js_to_num(vm, l), ipo_js_to_num(vm, r)));
	case TOK_BITAND:return ipo_js_num((double)(ipo_js_to_i32(vm, l) & ipo_js_to_i32(vm, r)));
	case TOK_BITOR: return ipo_js_num((double)(ipo_js_to_i32(vm, l) | ipo_js_to_i32(vm, r)));
	case TOK_BITXOR:return ipo_js_num((double)(ipo_js_to_i32(vm, l) ^ ipo_js_to_i32(vm, r)));
	case TOK_SHL:   return ipo_js_num((double)(ipo_js_to_i32(vm, l) << (ipo_js_to_i32(vm, r) & 31)));
	case TOK_SHR:   return ipo_js_num((double)(ipo_js_to_i32(vm, l) >> (ipo_js_to_i32(vm, r) & 31)));
	case TOK_USHR:  return ipo_js_num((double)((uint32_t)ipo_js_to_i32(vm, l) >> (ipo_js_to_i32(vm, r) & 31)));
	case TOK_EQ:    return ipo_js_bool(js_loose_eq(vm, l, r));
	case TOK_NEQ:   return ipo_js_bool(!js_loose_eq(vm, l, r));
	case TOK_SEQ:   return ipo_js_bool(js_strict_eq(l, r));
	case TOK_SNEQ:  return ipo_js_bool(!js_strict_eq(l, r));
	case TOK_LT:
	case TOK_LE:
	case TOK_GT:
	case TOK_GE: {
		if (l.type == JS_T_OBJECT) l = to_primitive(vm, l, 0);
		if (r.type == JS_T_OBJECT) r = to_primitive(vm, r, 0);
		if (l.type == JS_T_STRING && r.type == JS_T_STRING) {
			int cmp = strcmp(l.u.str->data, r.u.str->data);
			if (op == TOK_LT) return ipo_js_bool(cmp < 0);
			if (op == TOK_LE) return ipo_js_bool(cmp <= 0);
			if (op == TOK_GT) return ipo_js_bool(cmp > 0);
			return ipo_js_bool(cmp >= 0);
		}
		double a = ipo_js_to_num(vm, l), b = ipo_js_to_num(vm, r);
		if (op == TOK_LT) return ipo_js_bool(a < b);
		if (op == TOK_LE) return ipo_js_bool(a <= b);
		if (op == TOK_GT) return ipo_js_bool(a > b);
		return ipo_js_bool(a >= b);
	}
	case TOK_IN: {
		if (r.type != JS_T_OBJECT || !r.u.obj) {
			ipo_js_throw_msg(vm, "TypeError", "Cannot use 'in' operator on non-object");
			return ipo_js_bool(0);
		}
		const char *k = ipo_js_cstr(vm, l, NULL);
		for (ipo_js_obj_t *cur = r.u.obj; cur; cur = cur->proto) {
			if (ipo_js_has_own_prop(cur, k)) return ipo_js_bool(1);
			if (cur->host_get) {
				ipo_js_val_t hv;
				if (cur->host_get(vm, cur, k, &hv)) return ipo_js_bool(1);
			}
		}
		return ipo_js_bool(0);
	}
	case TOK_INSTANCEOF: {
		if (l.type != JS_T_OBJECT || !l.u.obj || r.type != JS_T_OBJECT || !r.u.obj)
			return ipo_js_bool(0);
		ipo_js_val_t rproto = ipo_js_undef();
		ipo_js_get_prop(vm, r.u.obj, "prototype", &rproto);
		if (rproto.type != JS_T_OBJECT || !rproto.u.obj) return ipo_js_bool(0);
		for (ipo_js_obj_t *cur = l.u.obj->proto; cur; cur = cur->proto) {
			if (cur == rproto.u.obj) return ipo_js_bool(1);
		}
		return ipo_js_bool(0);
	}
	}
	return ipo_js_undef();
}

static void eval_arg_list(ipo_js_vm_t *vm, ipo_js_env_t *env, const js_ast_list_t *args,
                          ipo_js_val_t **out_argv, int *out_argc) {
	int cap = (int)args->len + 4;
	ipo_js_val_t *argv = (ipo_js_val_t *)xmalloc((size_t)cap * sizeof(ipo_js_val_t));
	int argc = 0;
	for (size_t i = 0; i < args->len; i++) {
		ipo_js_ast_t *a = args->items[i];
		if (a->kind == AST_SPREAD) {
			ipo_js_val_t sv = vm_eval_expr(vm, env, a->u.spread.arg);
			if (vm->sig != SIG_NONE) { free(argv); *out_argv = NULL; *out_argc = 0; return; }
			if (sv.type == JS_T_OBJECT && sv.u.obj && sv.u.obj->kind == JS_OBJ_ARRAY) {
				for (size_t j = 0; j < sv.u.obj->u.arr.len; j++) {
					if (argc >= cap) { cap *= 2; argv = (ipo_js_val_t *)xrealloc(argv, (size_t)cap * sizeof(ipo_js_val_t)); }
					argv[argc++] = sv.u.obj->u.arr.items[j];
					ipo_js_gc_push_root(vm, argv[argc - 1]);
				}
			}
		} else {
			ipo_js_val_t v = vm_eval_expr(vm, env, a);
			if (vm->sig != SIG_NONE) { free(argv); *out_argv = NULL; *out_argc = 0; return; }
			if (argc >= cap) { cap *= 2; argv = (ipo_js_val_t *)xrealloc(argv, (size_t)cap * sizeof(ipo_js_val_t)); }
			argv[argc++] = v;
			ipo_js_gc_push_root(vm, v);
		}
	}
	*out_argv = argv;
	*out_argc = argc;
}

static ipo_js_val_t vm_call_obj(ipo_js_vm_t *vm, ipo_js_val_t callee, ipo_js_val_t this_val,
                                int argc, const ipo_js_val_t *argv, ipo_js_obj_t *super_proto) {
	if (vm->call_depth >= vm->max_depth) {
		ipo_js_throw_msg(vm, "RangeError", "Maximum call stack size exceeded");
		return ipo_js_undef();
	}
	if (!ipo_js_is_callable(callee)) {
		ipo_js_throw_msg(vm, "TypeError", "%s is not a function",
		                 callee.type == JS_T_UNDEFINED ? "undefined" : ipo_js_cstr(vm, callee, NULL));
		return ipo_js_undef();
	}

	ipo_js_obj_t *fn = callee.u.obj;
	size_t top = ipo_js_gc_push_root(vm, callee);
	ipo_js_gc_push_root(vm, this_val);
	for (int i = 0; i < argc; i++) ipo_js_gc_push_root(vm, argv[i]);

	if (fn->kind == JS_OBJ_NATIVE_FN) {
		ipo_js_val_t use_this = fn->u.nat.is_bound ? fn->u.nat.bound_this : this_val;
		ipo_js_val_t *use_args = (ipo_js_val_t *)argv;
		int use_argc = argc;
		ipo_js_val_t *merged = NULL;
		if (fn->u.nat.is_bound && fn->u.nat.bound_argc > 0) {
			use_argc = fn->u.nat.bound_argc + argc;
			merged = (ipo_js_val_t *)xmalloc((size_t)use_argc * sizeof(ipo_js_val_t));
			memcpy(merged, fn->u.nat.bound_args, (size_t)fn->u.nat.bound_argc * sizeof(ipo_js_val_t));
			if (argc > 0) memcpy(merged + fn->u.nat.bound_argc, argv, (size_t)argc * sizeof(ipo_js_val_t));
			use_args = merged;
		}
		vm->call_depth++;
		ipo_js_val_t res = fn->u.nat.fn(vm, use_this, use_argc, use_args, fn->u.nat.userdata);
		vm->call_depth--;
		free(merged);
		ipo_js_gc_pop_roots(vm, top);
		return res;
	}

	ipo_js_ast_t *ast = fn->u.fn.func_ast;
	ipo_js_val_t use_this = fn->u.fn.is_bound ? fn->u.fn.bound_this : this_val;
	if (!fn->u.fn.is_arrow && !fn->u.fn.is_bound &&
	    (use_this.type == JS_T_UNDEFINED || use_this.type == JS_T_NULL)) {
		use_this = ipo_js_obj_val(vm->global_obj);
	} else if (fn->u.fn.is_arrow && fn->u.fn.closure) {
		for (ipo_js_env_t *e = fn->u.fn.closure; e; e = e->parent) {
			if (e->has_this) { use_this = e->this_val; break; }
		}
	}

	ipo_js_val_t *use_args = (ipo_js_val_t *)argv;
	int use_argc = argc;
	ipo_js_val_t *merged = NULL;
	if (fn->u.fn.is_bound && fn->u.fn.bound_argc > 0) {
		use_argc = fn->u.fn.bound_argc + argc;
		merged = (ipo_js_val_t *)xmalloc((size_t)use_argc * sizeof(ipo_js_val_t));
		memcpy(merged, fn->u.fn.bound_args, (size_t)fn->u.fn.bound_argc * sizeof(ipo_js_val_t));
		if (argc > 0) memcpy(merged + fn->u.fn.bound_argc, argv, (size_t)argc * sizeof(ipo_js_val_t));
		use_args = merged;
	}

	ipo_js_env_t *fenv = vm_alloc_env(vm, fn->u.fn.closure ? fn->u.fn.closure : vm->global_env);
	fenv->is_func_scope = 1;
	if (!fn->u.fn.is_arrow) {
		fenv->has_this = 1;
		fenv->this_val = use_this;
	}

	if (vm->frame_len >= vm->frame_cap) {
		int ncap = vm->frame_cap ? vm->frame_cap * 2 : 16;
		vm->frames = (js_call_frame_t *)xrealloc(vm->frames, (size_t)ncap * sizeof(js_call_frame_t));
		vm->frame_cap = ncap;
	}
	vm->frames[vm->frame_len++] = (js_call_frame_t){
		.env = fenv, .this_val = use_this, .super_proto = super_proto, .active_fn = fn
	};
	vm->call_depth++;

	/* Bind arguments object */
	if (!fn->u.fn.is_arrow) {
		ipo_js_obj_t *args_arr = ipo_js_new_array(vm, (size_t)use_argc);
		for (int i = 0; i < use_argc; i++) args_arr->u.arr.items[i] = use_args[i];
		env_def(vm, fenv, "arguments", ipo_js_obj_val(args_arr), 0);
	}

	/* Bind parameters */
	for (size_t i = 0; i < ast->u.fn.param_len; i++) {
		js_param_ast_t *pa = &ast->u.fn.params[i];
		if (pa->is_rest) {
			ipo_js_obj_t *rest = ipo_js_new_array(vm, 0);
			for (int j = (int)i; j < use_argc; j++) ipo_js_arr_push(vm, rest, use_args[j]);
			bind_or_assign_pattern(vm, fenv, pa->pattern, ipo_js_obj_val(rest), 1);
			break;
		}
		ipo_js_val_t pv = ((int)i < use_argc) ? use_args[i] : ipo_js_undef();
		if (pv.type == JS_T_UNDEFINED && pa->def_val) {
			pv = vm_eval_expr(vm, fenv, pa->def_val);
			if (vm->sig != SIG_NONE) break;
		}
		bind_or_assign_pattern(vm, fenv, pa->pattern, pv, 1);
	}

	ipo_js_val_t ret = ipo_js_undef();
	if (vm->sig == SIG_NONE) {
		hoist_block(vm, fenv, fenv, ast->u.fn.body, 1);
		vm_exec_stmt(vm, fenv, ast->u.fn.body);
		if (vm->sig == SIG_RETURN) {
			ret = vm->sig_val;
			vm->sig = SIG_NONE;
			vm->sig_val = ipo_js_undef();
		}
	}

	if (ast->u.fn.is_async) {
		ipo_js_obj_t *p = ipo_js_new_promise(vm);
		if (vm->sig == SIG_THROW) {
			ipo_js_promise_reject(vm, p, vm->sig_val);
			ipo_js_clear_exception(vm);
		} else {
			ipo_js_promise_resolve(vm, p, ret);
		}
		ret = ipo_js_obj_val(p);
	}

	vm->call_depth--;
	vm->frame_len--;
	free(merged);
	ipo_js_gc_pop_roots(vm, top);
	return ret;
}

static ipo_js_val_t vm_construct_obj(ipo_js_vm_t *vm, ipo_js_val_t ctor,
                                     int argc, const ipo_js_val_t *argv) {
	if (!ipo_js_is_callable(ctor)) {
		ipo_js_throw_msg(vm, "TypeError", "Value is not a constructor");
		return ipo_js_undef();
	}
	ipo_js_obj_t *fn = ctor.u.obj;
	if (fn->kind == JS_OBJ_NATIVE_FN) {
		ipo_js_val_t pv = ipo_js_undef();
		ipo_js_get_prop(vm, fn, "prototype", &pv);
		ipo_js_obj_t *inst = ipo_js_new_object(vm, (pv.type == JS_T_OBJECT && pv.u.obj) ? pv.u.obj : vm->obj_proto);
		size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(inst));
		ipo_js_val_t r = vm_call_obj(vm, ctor, ipo_js_obj_val(inst), argc, argv, NULL);
		ipo_js_gc_pop_roots(vm, top);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		return (r.type == JS_T_OBJECT && r.u.obj) ? r : ipo_js_obj_val(inst);
	}
	ipo_js_val_t pv = ipo_js_undef();
	ipo_js_get_prop(vm, fn, "prototype", &pv);
	ipo_js_obj_t *proto = (pv.type == JS_T_OBJECT && pv.u.obj) ? pv.u.obj : vm->obj_proto;
	ipo_js_obj_t *inst = ipo_js_new_object(vm, proto);
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(inst));
	ipo_js_val_t r = vm_call_obj(vm, ctor, ipo_js_obj_val(inst), argc, argv, proto->proto);
	ipo_js_gc_pop_roots(vm, top);
	if (vm->sig != SIG_NONE) return ipo_js_undef();
	return (r.type == JS_T_OBJECT && r.u.obj) ? r : ipo_js_obj_val(inst);
}

static ipo_js_val_t eval_class_node(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *cls) {
	ipo_js_obj_t *super_ctor = NULL;
	ipo_js_obj_t *super_proto = vm->obj_proto;
	if (cls->u.cls.super_expr) {
		ipo_js_val_t sv = vm_eval_expr(vm, env, cls->u.cls.super_expr);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		if (ipo_js_is_callable(sv)) {
			super_ctor = sv.u.obj;
			ipo_js_val_t sp = ipo_js_undef();
			ipo_js_get_prop(vm, super_ctor, "prototype", &sp);
			if (sp.type == JS_T_OBJECT && sp.u.obj) super_proto = sp.u.obj;
		}
	}

	ipo_js_obj_t *proto = ipo_js_new_object(vm, super_proto);
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(proto));
	ipo_js_ast_t *ctor_ast = NULL;
	for (size_t i = 0; i < cls->u.cls.elem_len; i++) {
		js_class_elem_t *el = &cls->u.cls.elems[i];
		if (!el->is_static && el->kind == 0 && el->name && strcmp(el->name, "constructor") == 0) {
			ctor_ast = el->fn_ast;
		}
	}
	if (!ctor_ast) {
		static js_ast_pool_t empty_pool_unused;
		(void)empty_pool_unused;
		/* Fallback empty native constructor */
	}
	ipo_js_obj_t *ctor_obj = NULL;
	if (ctor_ast) {
		ctor_obj = vm_new_js_fn(vm, ctor_ast, env);
		if (cls->u.cls.name) ctor_obj->u.fn.name = vm_alloc_str(vm, cls->u.cls.name, strlen(cls->u.cls.name));
	} else {
		ctor_obj = vm_alloc_obj(vm, JS_OBJ_FUNCTION, vm->fn_proto);
		ipo_js_ast_t *dummy_fn = (ipo_js_ast_t *)xcalloc(1, sizeof(ipo_js_ast_t));
		ipo_js_ast_t *dummy_blk = (ipo_js_ast_t *)xcalloc(1, sizeof(ipo_js_ast_t));
		dummy_fn->kind = AST_FUNC;
		dummy_blk->kind = AST_BLOCK;
		dummy_fn->u.fn.body = dummy_blk;
		/* Attach to VM pool so freed on vm_free */
		js_ast_pool_t *dp = (js_ast_pool_t *)xcalloc(1, sizeof(js_ast_pool_t));
		dp->nodes = (ipo_js_ast_t **)xmalloc(2 * sizeof(ipo_js_ast_t *));
		dp->nodes[0] = dummy_fn; dp->nodes[1] = dummy_blk;
		dp->len = 2; dp->cap = 2;
		vm->ast_pools = (js_ast_pool_t **)xrealloc(vm->ast_pools, (vm->ast_pool_len + 1) * sizeof(js_ast_pool_t *));
		vm->ast_pools[vm->ast_pool_len++] = dp;
		ctor_obj->u.fn.func_ast = dummy_fn;
		ctor_obj->u.fn.closure = env;
		ctor_obj->u.fn.name = vm_alloc_str(vm, cls->u.cls.name ? cls->u.cls.name : "",
		                                   cls->u.cls.name ? strlen(cls->u.cls.name) : 0);
	}
	if (super_ctor) ctor_obj->proto = super_ctor;
	ipo_js_set_prop(vm, ctor_obj, "prototype", ipo_js_obj_val(proto));
	ipo_js_set_prop(vm, proto, "constructor", ipo_js_obj_val(ctor_obj));
	ipo_js_gc_push_root(vm, ipo_js_obj_val(ctor_obj));

	for (size_t i = 0; i < cls->u.cls.elem_len; i++) {
		js_class_elem_t *el = &cls->u.cls.elems[i];
		if (!el->is_static && el->kind == 0 && el->name && strcmp(el->name, "constructor") == 0) continue;
		const char *kname = el->name;
		if (!kname && el->key_ast) {
			ipo_js_val_t kv = vm_eval_expr(vm, env, el->key_ast);
			if (vm->sig != SIG_NONE) break;
			kname = ipo_js_cstr(vm, kv, NULL);
		}
		if (!kname) continue;
		ipo_js_obj_t *mfn = vm_new_js_fn(vm, el->fn_ast, env);
		ipo_js_obj_t *dst = el->is_static ? ctor_obj : proto;
		if (el->kind == 1) obj_define_accessor(vm, dst, kname, mfn, NULL);
		else if (el->kind == 2) obj_define_accessor(vm, dst, kname, NULL, mfn);
		else ipo_js_set_prop(vm, dst, kname, ipo_js_obj_val(mfn));
	}

	if (cls->u.cls.is_decl && cls->u.cls.name) {
		env_def(vm, env, cls->u.cls.name, ipo_js_obj_val(ctor_obj), 0);
	}
	ipo_js_gc_pop_roots(vm, top);
	return ipo_js_obj_val(ctor_obj);
}

static ipo_js_val_t vm_eval_expr_inner(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *ast);
static ipo_js_val_t vm_eval_expr(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *ast) {
	if (!ast || vm->sig != SIG_NONE) return ipo_js_undef();
	if (vm->eval_depth >= 384) {
		ipo_js_throw_msg(vm, "RangeError", "Maximum expression evaluation depth exceeded");
		return ipo_js_undef();
	}
	vm->eval_depth++;
	ipo_js_val_t r = vm_eval_expr_inner(vm, env, ast);
	vm->eval_depth--;
	return r;
}

static ipo_js_val_t vm_eval_expr_inner(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *ast) {
	if (!ast || vm->sig != SIG_NONE) return ipo_js_undef();
	if (++vm->steps > vm->max_steps) {
		ipo_js_throw_msg(vm, "RangeError", "Script execution step limit exceeded");
		return ipo_js_undef();
	}
	gc_maybe_collect(vm);

	switch (ast->kind) {
	case AST_UNDEF: return ipo_js_undef();
	case AST_NULL:  return ipo_js_null();
	case AST_BOOL:  return ipo_js_bool(ast->u.b_lit.b);
	case AST_NUM:   return ipo_js_num(ast->u.n_lit.num);
	case AST_STR:   return ipo_js_str_new(vm, ast->u.s_lit.str, ast->u.s_lit.len);
	case AST_REGEXP:
		return ipo_js_obj_val(vm_new_regexp(vm, ast->u.re_lit.pat,
		                                    strlen(ast->u.re_lit.pat),
		                                    parse_re_flags(ast->u.re_lit.flags)));
	case AST_TEMPLATE: {
		js_buf_t b;
		jbuf_init(&b);
		jbuf_append(&b, ast->u.tmpl.quasis[0], ast->u.tmpl.qlen[0]);
		for (size_t i = 0; i < ast->u.tmpl.count; i++) {
			ipo_js_val_t ev = vm_eval_expr(vm, env, ast->u.tmpl.exprs[i]);
			if (vm->sig != SIG_NONE) { jbuf_free(&b); return ipo_js_undef(); }
			size_t slen = 0;
			const char *s = ipo_js_cstr(vm, ev, &slen);
			jbuf_append(&b, s, slen);
			jbuf_append(&b, ast->u.tmpl.quasis[i + 1], ast->u.tmpl.qlen[i + 1]);
		}
		ipo_js_val_t out = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
		jbuf_free(&b);
		return out;
	}
	case AST_THIS: {
		for (ipo_js_env_t *e = env; e; e = e->parent) {
			if (e->has_this) return e->this_val;
		}
		return ipo_js_obj_val(vm->global_obj);
	}
	case AST_SUPER:
		if (vm->frame_len > 0 && vm->frames[vm->frame_len - 1].super_proto) {
			return ipo_js_obj_val(vm->frames[vm->frame_len - 1].super_proto);
		}
		return ipo_js_undef();
	case AST_IDENT: {
		ipo_js_val_t v;
		if (env_get(vm, env, ast->u.ident.name, &v)) return v;
		ipo_js_throw_msg(vm, "ReferenceError", "%s is not defined", ast->u.ident.name);
		return ipo_js_undef();
	}
	case AST_FUNC:
		return ipo_js_obj_val(vm_new_js_fn(vm, ast, env));
	case AST_CLASS:
		return eval_class_node(vm, env, ast);
	case AST_ARRAY: {
		ipo_js_obj_t *arr = ipo_js_new_array(vm, 0);
		size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(arr));
		for (size_t i = 0; i < ast->u.arr.elems.len; i++) {
			ipo_js_ast_t *item = ast->u.arr.elems.items[i];
			if (item->kind == AST_SPREAD) {
				ipo_js_val_t sv = vm_eval_expr(vm, env, item->u.spread.arg);
				if (vm->sig != SIG_NONE) break;
				if (sv.type == JS_T_OBJECT && sv.u.obj && sv.u.obj->kind == JS_OBJ_ARRAY) {
					for (size_t j = 0; j < sv.u.obj->u.arr.len; j++)
						ipo_js_arr_push(vm, arr, sv.u.obj->u.arr.items[j]);
				}
			} else {
				ipo_js_val_t v = vm_eval_expr(vm, env, item);
				if (vm->sig != SIG_NONE) break;
				ipo_js_arr_push(vm, arr, v);
			}
		}
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_obj_val(arr);
	}
	case AST_OBJECT: {
		ipo_js_obj_t *obj = ipo_js_new_object(vm, vm->obj_proto);
		size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(obj));
		for (size_t i = 0; i < ast->u.obj.len; i++) {
			js_obj_prop_ast_t *pr = &ast->u.obj.props[i];
			if (pr->kind == 3) {
				ipo_js_val_t sv = vm_eval_expr(vm, env, pr->val);
				if (vm->sig != SIG_NONE) break;
				if (sv.type == JS_T_OBJECT && sv.u.obj) {
					for (size_t j = 0; j < sv.u.obj->prop_len; j++) {
						if (sv.u.obj->props[j].flags & 0x01)
							ipo_js_set_prop(vm, obj, sv.u.obj->props[j].key->data, sv.u.obj->props[j].val);
					}
				}
				continue;
			}
			const char *kname = pr->key_name;
			ipo_js_val_t kroot = ipo_js_undef();
			if (!kname && pr->key_ast) {
				kroot = vm_eval_expr(vm, env, pr->key_ast);
				if (vm->sig != SIG_NONE) break;
				ipo_js_gc_push_root(vm, kroot);
				kname = ipo_js_cstr(vm, kroot, NULL);
			}
			ipo_js_val_t val = vm_eval_expr(vm, env, pr->val);
			if (vm->sig != SIG_NONE) break;
			if (kname) {
				if (pr->kind == 1 && val.type == JS_T_OBJECT)
					obj_define_accessor(vm, obj, kname, val.u.obj, NULL);
				else if (pr->kind == 2 && val.type == JS_T_OBJECT)
					obj_define_accessor(vm, obj, kname, NULL, val.u.obj);
				else
					ipo_js_set_prop(vm, obj, kname, val);
			}
		}
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_obj_val(obj);
	}
	case AST_MEMBER: {
		ipo_js_val_t obj = vm_eval_expr(vm, env, ast->u.mem.obj);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		if (ast->u.mem.optional && (obj.type == JS_T_UNDEFINED || obj.type == JS_T_NULL)) {
			return ipo_js_undef();
		}
		const char *k = ast->u.mem.prop_name;
		if (ast->u.mem.computed) {
			ipo_js_val_t kv = vm_eval_expr(vm, env, ast->u.mem.prop_expr);
			if (vm->sig != SIG_NONE) return ipo_js_undef();
			k = ipo_js_cstr(vm, kv, NULL);
		}
		ipo_js_val_t out = ipo_js_undef();
		vm_get_prop_val(vm, obj, k, &out);
		return out;
	}
	case AST_CALL: {
		ipo_js_val_t this_val = ipo_js_undef();
		ipo_js_val_t callee = ipo_js_undef();
		ipo_js_obj_t *super_proto = NULL;
		size_t top = ipo_js_gc_push_root(vm, ipo_js_undef());

		if (ast->u.call.callee->kind == AST_SUPER) {
			/* super(args) in subclass constructor */
			if (vm->frame_len > 0 && vm->frames[vm->frame_len - 1].super_proto) {
				super_proto = vm->frames[vm->frame_len - 1].super_proto;
				this_val = vm->frames[vm->frame_len - 1].this_val;
				ipo_js_get_prop(vm, super_proto, "constructor", &callee);
			}
		} else if (ast->u.call.callee->kind == AST_MEMBER) {
			ipo_js_ast_t *m = ast->u.call.callee;
			if (m->u.mem.obj->kind == AST_SUPER && vm->frame_len > 0 && vm->frames[vm->frame_len - 1].super_proto) {
				ipo_js_obj_t *sp = vm->frames[vm->frame_len - 1].super_proto;
				this_val = vm->frames[vm->frame_len - 1].this_val;
				const char *k = m->u.mem.computed
				                    ? ipo_js_cstr(vm, vm_eval_expr(vm, env, m->u.mem.prop_expr), NULL)
				                    : m->u.mem.prop_name;
				ipo_js_get_prop(vm, sp, k, &callee);
			} else {
				this_val = vm_eval_expr(vm, env, m->u.mem.obj);
				if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
				ipo_js_gc_push_root(vm, this_val);
				if ((m->u.mem.optional || ast->u.call.optional) &&
				    (this_val.type == JS_T_UNDEFINED || this_val.type == JS_T_NULL)) {
					ipo_js_gc_pop_roots(vm, top);
					return ipo_js_undef();
				}
				const char *k = m->u.mem.prop_name;
				if (m->u.mem.computed) {
					ipo_js_val_t kv = vm_eval_expr(vm, env, m->u.mem.prop_expr);
					if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
					k = ipo_js_cstr(vm, kv, NULL);
				}
				vm_get_prop_val(vm, this_val, k, &callee);
			}
		} else {
			callee = vm_eval_expr(vm, env, ast->u.call.callee);
		}
		if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
		if (ast->u.call.optional && (callee.type == JS_T_UNDEFINED || callee.type == JS_T_NULL)) {
			ipo_js_gc_pop_roots(vm, top);
			return ipo_js_undef();
		}
		ipo_js_val_t *argv = NULL;
		int argc = 0;
		eval_arg_list(vm, env, &ast->u.call.args, &argv, &argc);
		if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
		ipo_js_val_t res = vm_call_obj(vm, callee, this_val, argc, argv, super_proto);
		free(argv);
		ipo_js_gc_pop_roots(vm, top);
		return res;
	}
	case AST_NEW: {
		size_t top = ipo_js_gc_push_root(vm, ipo_js_undef());
		ipo_js_val_t ctor = vm_eval_expr(vm, env, ast->u.call.callee);
		if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
		ipo_js_gc_push_root(vm, ctor);
		ipo_js_val_t *argv = NULL;
		int argc = 0;
		eval_arg_list(vm, env, &ast->u.call.args, &argv, &argc);
		if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
		ipo_js_val_t res = vm_construct_obj(vm, ctor, argc, argv);
		free(argv);
		ipo_js_gc_pop_roots(vm, top);
		return res;
	}
	case AST_UNARY: {
		int op = ast->u.unary.op;
		if (op == TOK_TYPEOF) {
			if (ast->u.unary.arg->kind == AST_IDENT) {
				ipo_js_val_t v;
				if (!env_get(vm, env, ast->u.unary.arg->u.ident.name, &v))
					return ipo_js_str_cstr(vm, "undefined");
				switch (v.type) {
				case JS_T_UNDEFINED: return ipo_js_str_cstr(vm, "undefined");
				case JS_T_NULL:      return ipo_js_str_cstr(vm, "object");
				case JS_T_BOOL:      return ipo_js_str_cstr(vm, "boolean");
				case JS_T_NUMBER:    return ipo_js_str_cstr(vm, "number");
				case JS_T_STRING:    return ipo_js_str_cstr(vm, "string");
				case JS_T_OBJECT:    return ipo_js_str_cstr(vm, ipo_js_is_callable(v) ? "function" : "object");
				}
			}
			ipo_js_val_t v = vm_eval_expr(vm, env, ast->u.unary.arg);
			if (vm->sig != SIG_NONE) return ipo_js_undef();
			switch (v.type) {
			case JS_T_UNDEFINED: return ipo_js_str_cstr(vm, "undefined");
			case JS_T_NULL:      return ipo_js_str_cstr(vm, "object");
			case JS_T_BOOL:      return ipo_js_str_cstr(vm, "boolean");
			case JS_T_NUMBER:    return ipo_js_str_cstr(vm, "number");
			case JS_T_STRING:    return ipo_js_str_cstr(vm, "string");
			case JS_T_OBJECT:    return ipo_js_str_cstr(vm, ipo_js_is_callable(v) ? "function" : "object");
			}
		}
		if (op == TOK_DELETE) {
			if (ast->u.unary.arg->kind == AST_MEMBER) {
				ipo_js_ast_t *m = ast->u.unary.arg;
				ipo_js_val_t obj = vm_eval_expr(vm, env, m->u.mem.obj);
				if (vm->sig != SIG_NONE) return ipo_js_bool(0);
				if (obj.type == JS_T_OBJECT && obj.u.obj) {
					const char *k = m->u.mem.computed
					                    ? ipo_js_cstr(vm, vm_eval_expr(vm, env, m->u.mem.prop_expr), NULL)
					                    : m->u.mem.prop_name;
					ipo_js_del_prop(obj.u.obj, k);
				}
			}
			return ipo_js_bool(1);
		}
		if (op == TOK_INC || op == TOK_DEC) {
			double d = ipo_js_to_num(vm, read_lvalue(vm, env, ast->u.unary.arg));
			if (vm->sig != SIG_NONE) return ipo_js_undef();
			ipo_js_val_t nv = ipo_js_num(op == TOK_INC ? d + 1.0 : d - 1.0);
			bind_or_assign_pattern(vm, env, ast->u.unary.arg, nv, -1);
			return nv;
		}
		ipo_js_val_t v = vm_eval_expr(vm, env, ast->u.unary.arg);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		if (op == TOK_VOID) return ipo_js_undef();
		if (op == TOK_NOT)  return ipo_js_bool(!ipo_js_to_bool(v));
		if (op == TOK_BITNOT) return ipo_js_num((double)(~ipo_js_to_i32(vm, v)));
		if (op == TOK_PLUS) return ipo_js_num(ipo_js_to_num(vm, v));
		if (op == TOK_MINUS) return ipo_js_num(-ipo_js_to_num(vm, v));
		if (op == TOK_AWAIT) {
			vm_drain_microtasks(vm);
			if (v.type == JS_T_OBJECT && v.u.obj && v.u.obj->kind == JS_OBJ_PROMISE) {
				if (v.u.obj->u.prom.state == 1) return v.u.obj->u.prom.result;
				if (v.u.obj->u.prom.state == 2) { ipo_js_throw(vm, v.u.obj->u.prom.result); return ipo_js_undef(); }
			}
			return v;
		}
		return ipo_js_undef();
	}
	case AST_POSTFIX: {
		double d = ipo_js_to_num(vm, read_lvalue(vm, env, ast->u.unary.arg));
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		ipo_js_val_t oldv = ipo_js_num(d);
		ipo_js_val_t newv = ipo_js_num(ast->u.unary.op == TOK_INC ? d + 1.0 : d - 1.0);
		bind_or_assign_pattern(vm, env, ast->u.unary.arg, newv, -1);
		return oldv;
	}
	case AST_BINARY: {
		int op = ast->u.bin.op;
		if (op == TOK_AND) {
			ipo_js_val_t l = vm_eval_expr(vm, env, ast->u.bin.left);
			if (vm->sig != SIG_NONE || !ipo_js_to_bool(l)) return l;
			return vm_eval_expr(vm, env, ast->u.bin.right);
		}
		if (op == TOK_OR) {
			ipo_js_val_t l = vm_eval_expr(vm, env, ast->u.bin.left);
			if (vm->sig != SIG_NONE || ipo_js_to_bool(l)) return l;
			return vm_eval_expr(vm, env, ast->u.bin.right);
		}
		if (op == TOK_NULLISH) {
			ipo_js_val_t l = vm_eval_expr(vm, env, ast->u.bin.left);
			if (vm->sig != SIG_NONE) return ipo_js_undef();
			if (l.type != JS_T_UNDEFINED && l.type != JS_T_NULL) return l;
			return vm_eval_expr(vm, env, ast->u.bin.right);
		}
		size_t top = ipo_js_gc_push_root(vm, ipo_js_undef());
		ipo_js_val_t l = vm_eval_expr(vm, env, ast->u.bin.left);
		if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
		ipo_js_gc_push_root(vm, l);
		ipo_js_val_t r = vm_eval_expr(vm, env, ast->u.bin.right);
		if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
		ipo_js_val_t res = apply_binop(vm, op, l, r);
		ipo_js_gc_pop_roots(vm, top);
		return res;
	}
	case AST_TERNARY: {
		ipo_js_val_t cond = vm_eval_expr(vm, env, ast->u.tern.test);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		return vm_eval_expr(vm, env, ipo_js_to_bool(cond) ? ast->u.tern.cons : ast->u.tern.alt);
	}
	case AST_ASSIGN: {
		int op = ast->u.bin.op;
		if (op == TOK_ASSIGN) {
			ipo_js_val_t r = vm_eval_expr(vm, env, ast->u.bin.right);
			if (vm->sig != SIG_NONE) return ipo_js_undef();
			bind_or_assign_pattern(vm, env, ast->u.bin.left, r, -1);
			return r;
		}
		ipo_js_val_t cur = read_lvalue(vm, env, ast->u.bin.left);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		if (op == TOK_LOGAND_ASSIGN && !ipo_js_to_bool(cur)) return cur;
		if (op == TOK_LOGOR_ASSIGN && ipo_js_to_bool(cur)) return cur;
		if (op == TOK_NULLISH_ASSIGN && cur.type != JS_T_UNDEFINED && cur.type != JS_T_NULL) return cur;

		size_t top = ipo_js_gc_push_root(vm, cur);
		ipo_js_val_t r = vm_eval_expr(vm, env, ast->u.bin.right);
		if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }

		ipo_js_val_t nv = r;
		if (op != TOK_LOGAND_ASSIGN && op != TOK_LOGOR_ASSIGN && op != TOK_NULLISH_ASSIGN) {
			int binop = TOK_PLUS;
			switch (op) {
			case TOK_ADD_ASSIGN:  binop = TOK_PLUS; break;
			case TOK_SUB_ASSIGN:  binop = TOK_MINUS; break;
			case TOK_MUL_ASSIGN:  binop = TOK_MUL; break;
			case TOK_DIV_ASSIGN:  binop = TOK_DIV; break;
			case TOK_MOD_ASSIGN:  binop = TOK_MOD; break;
			case TOK_POW_ASSIGN:  binop = TOK_POW; break;
			case TOK_AND_ASSIGN:  binop = TOK_BITAND; break;
			case TOK_OR_ASSIGN:   binop = TOK_BITOR; break;
			case TOK_XOR_ASSIGN:  binop = TOK_BITXOR; break;
			case TOK_SHL_ASSIGN:  binop = TOK_SHL; break;
			case TOK_SHR_ASSIGN:  binop = TOK_SHR; break;
			case TOK_USHR_ASSIGN: binop = TOK_USHR; break;
			}
			nv = apply_binop(vm, binop, cur, r);
		}
		bind_or_assign_pattern(vm, env, ast->u.bin.left, nv, -1);
		ipo_js_gc_pop_roots(vm, top);
		return nv;
	}
	case AST_SEQ: {
		ipo_js_val_t last = ipo_js_undef();
		for (size_t i = 0; i < ast->u.seq.exprs.len; i++) {
			last = vm_eval_expr(vm, env, ast->u.seq.exprs.items[i]);
			if (vm->sig != SIG_NONE) break;
		}
		return last;
	}
	default:
		return ipo_js_undef();
	}
}

static int signal_matches_label(ipo_js_vm_t *vm, const char *active_label) {
	if (!vm->sig_label) return 1;
	return active_label && strcmp(vm->sig_label, active_label) == 0;
}

static ipo_js_val_t vm_exec_stmt_inner(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *ast);
static ipo_js_val_t vm_exec_stmt(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *ast) {
	if (!ast || vm->sig != SIG_NONE) return ipo_js_undef();
	if (vm->eval_depth >= 384) {
		ipo_js_throw_msg(vm, "RangeError", "Maximum statement nesting depth exceeded");
		return ipo_js_undef();
	}
	vm->eval_depth++;
	ipo_js_val_t r = vm_exec_stmt_inner(vm, env, ast);
	vm->eval_depth--;
	return r;
}

static ipo_js_val_t vm_exec_stmt_inner(ipo_js_vm_t *vm, ipo_js_env_t *env, ipo_js_ast_t *ast) {
	if (!ast || vm->sig != SIG_NONE) return ipo_js_undef();
	if (++vm->steps > vm->max_steps) {
		ipo_js_throw_msg(vm, "RangeError", "Script execution step limit exceeded");
		return ipo_js_undef();
	}

	switch (ast->kind) {
	case AST_EMPTY:
		return ipo_js_undef();
	case AST_EXPR_STMT:
		return vm_eval_expr(vm, env, ast->u.expr_stmt.expr);
	case AST_FUNC:
		if (ast->u.fn.is_decl && ast->u.fn.name) {
			ipo_js_obj_t *fn = vm_new_js_fn(vm, ast, env);
			env_def(vm, env, ast->u.fn.name, ipo_js_obj_val(fn), 0);
		}
		return ipo_js_undef();
	case AST_CLASS:
		return eval_class_node(vm, env, ast);
	case AST_VAR_DECL:
		for (size_t i = 0; i < ast->u.var_decl.len; i++) {
			ipo_js_val_t v = ast->u.var_decl.inits[i]
			                     ? vm_eval_expr(vm, env, ast->u.var_decl.inits[i])
			                     : ipo_js_undef();
			if (vm->sig != SIG_NONE) break;
			if (ast->u.var_decl.decl_kind == 0 && !ast->u.var_decl.inits[i]) continue;
			bind_or_assign_pattern(vm, env, ast->u.var_decl.targets[i], v, ast->u.var_decl.decl_kind);
			if (vm->sig != SIG_NONE) break;
		}
		return ipo_js_undef();
	case AST_BLOCK: {
		ipo_js_env_t *benv = vm_alloc_env(vm, env);
		hoist_block(vm, env_var_scope(env), benv, ast, 0);
		ipo_js_val_t last = ipo_js_undef();
		for (size_t i = 0; i < ast->u.block.stmts.len; i++) {
			ipo_js_val_t r = vm_exec_stmt(vm, benv, ast->u.block.stmts.items[i]);
			if (vm->sig != SIG_NONE) break;
			if (ast->u.block.stmts.items[i]->kind == AST_EXPR_STMT) last = r;
		}
		return last;
	}
	case AST_IF: {
		ipo_js_val_t cond = vm_eval_expr(vm, env, ast->u.if_s.test);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		if (ipo_js_to_bool(cond)) return vm_exec_stmt(vm, env, ast->u.if_s.cons);
		if (ast->u.if_s.alt) return vm_exec_stmt(vm, env, ast->u.if_s.alt);
		return ipo_js_undef();
	}
	case AST_WHILE: {
		for (;;) {
			ipo_js_val_t cond = vm_eval_expr(vm, env, ast->u.loop_s.test);
			if (vm->sig != SIG_NONE || !ipo_js_to_bool(cond)) break;
			vm_exec_stmt(vm, env, ast->u.loop_s.body);
			if (vm->sig == SIG_BREAK && signal_matches_label(vm, NULL)) {
				vm->sig = SIG_NONE; free(vm->sig_label); vm->sig_label = NULL; break;
			}
			if (vm->sig == SIG_CONTINUE && signal_matches_label(vm, NULL)) {
				vm->sig = SIG_NONE; free(vm->sig_label); vm->sig_label = NULL; continue;
			}
			if (vm->sig != SIG_NONE) break;
		}
		return ipo_js_undef();
	}
	case AST_DO_WHILE: {
		for (;;) {
			vm_exec_stmt(vm, env, ast->u.loop_s.body);
			if (vm->sig == SIG_BREAK && signal_matches_label(vm, NULL)) {
				vm->sig = SIG_NONE; free(vm->sig_label); vm->sig_label = NULL; break;
			}
			if (vm->sig == SIG_CONTINUE && signal_matches_label(vm, NULL)) {
				vm->sig = SIG_NONE; free(vm->sig_label); vm->sig_label = NULL;
			} else if (vm->sig != SIG_NONE) break;
			ipo_js_val_t cond = vm_eval_expr(vm, env, ast->u.loop_s.test);
			if (vm->sig != SIG_NONE || !ipo_js_to_bool(cond)) break;
		}
		return ipo_js_undef();
	}
	case AST_FOR: {
		ipo_js_env_t *fenv = vm_alloc_env(vm, env);
		int per_iter_let = (ast->u.for_s.init && ast->u.for_s.init->kind == AST_VAR_DECL &&
		                    ast->u.for_s.init->u.var_decl.decl_kind != 0);
		if (ast->u.for_s.init) {
			vm_exec_stmt(vm, fenv, ast->u.for_s.init);
			if (vm->sig != SIG_NONE) return ipo_js_undef();
		}
		for (;;) {
			ipo_js_env_t *cur_env = fenv;
			if (per_iter_let) {
				/* Copy lexical bindings into fresh per-iteration env so closures capture each iteration */
				ipo_js_env_t *iter = vm_alloc_env(vm, fenv->parent);
				for (size_t i = 0; i < fenv->len; i++) {
					env_def(vm, iter, fenv->vars[i].name->data, fenv->vars[i].val, fenv->vars[i].is_const);
				}
				cur_env = iter;
			}
			if (ast->u.for_s.test) {
				ipo_js_val_t cond = vm_eval_expr(vm, cur_env, ast->u.for_s.test);
				if (vm->sig != SIG_NONE || !ipo_js_to_bool(cond)) break;
			}
			vm_exec_stmt(vm, cur_env, ast->u.for_s.body);
			if (per_iter_let) {
				/* Sync mutated loop variables back before update step */
				for (size_t i = 0; i < cur_env->len && i < fenv->len; i++) fenv->vars[i].val = cur_env->vars[i].val;
			}
			if (vm->sig == SIG_BREAK && signal_matches_label(vm, NULL)) {
				vm->sig = SIG_NONE; free(vm->sig_label); vm->sig_label = NULL; break;
			}
			if (vm->sig == SIG_CONTINUE && signal_matches_label(vm, NULL)) {
				vm->sig = SIG_NONE; free(vm->sig_label); vm->sig_label = NULL;
			} else if (vm->sig != SIG_NONE) break;
			if (ast->u.for_s.upd) {
				vm_eval_expr(vm, per_iter_let ? cur_env : fenv, ast->u.for_s.upd);
				if (per_iter_let) {
					for (size_t i = 0; i < cur_env->len && i < fenv->len; i++) fenv->vars[i].val = cur_env->vars[i].val;
				}
				if (vm->sig != SIG_NONE) break;
			}
		}
		return ipo_js_undef();
	}
	case AST_FOR_IN:
	case AST_FOR_OF: {
		ipo_js_val_t rhs = vm_eval_expr(vm, env, ast->u.for_iter.rhs);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		if (rhs.type == JS_T_UNDEFINED || rhs.type == JS_T_NULL) return ipo_js_undef();

		/* Collect iteration values up front */
		ipo_js_obj_t *items = ipo_js_new_array(vm, 0);
		size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(items));
		if (ast->kind == AST_FOR_IN) {
			if (rhs.type == JS_T_OBJECT && rhs.u.obj) {
				if (rhs.u.obj->kind == JS_OBJ_ARRAY) {
					for (size_t i = 0; i < rhs.u.obj->u.arr.len; i++) {
						char nb[24]; snprintf(nb, sizeof(nb), "%lu", (unsigned long)i);
						ipo_js_arr_push(vm, items, ipo_js_str_cstr(vm, nb));
					}
				}
				for (ipo_js_obj_t *o = rhs.u.obj; o; o = o->proto) {
					if (o == vm->obj_proto || o == vm->arr_proto) break;
					for (size_t i = 0; i < o->prop_len; i++) {
						if (o->props[i].flags & 0x01)
							ipo_js_arr_push(vm, items, (ipo_js_val_t){.type = JS_T_STRING, .u.str = o->props[i].key});
					}
				}
			}
		} else {
			if (rhs.type == JS_T_STRING && rhs.u.str) {
				for (size_t i = 0; i < rhs.u.str->len; i++)
					ipo_js_arr_push(vm, items, ipo_js_str_new(vm, &rhs.u.str->data[i], 1));
			} else if (rhs.type == JS_T_OBJECT && rhs.u.obj) {
				if (rhs.u.obj->kind == JS_OBJ_ARRAY) {
					for (size_t i = 0; i < rhs.u.obj->u.arr.len; i++)
						ipo_js_arr_push(vm, items, rhs.u.obj->u.arr.items[i]);
				} else {
					ipo_js_val_t lv = ipo_js_undef();
					if (ipo_js_get_prop(vm, rhs.u.obj, "length", &lv) && lv.type == JS_T_NUMBER) {
						size_t n = (size_t)lv.u.num;
						for (size_t i = 0; i < n; i++) {
							char nb[24]; snprintf(nb, sizeof(nb), "%lu", (unsigned long)i);
							ipo_js_val_t ev = ipo_js_undef();
							ipo_js_get_prop(vm, rhs.u.obj, nb, &ev);
							ipo_js_arr_push(vm, items, ev);
						}
					}
				}
			}
		}

		for (size_t i = 0; i < items->u.arr.len; i++) {
			ipo_js_env_t *ienv = (ast->u.for_iter.decl_kind >= 1) ? vm_alloc_env(vm, env) : env;
			bind_or_assign_pattern(vm, ienv, ast->u.for_iter.lhs, items->u.arr.items[i], ast->u.for_iter.decl_kind);
			if (vm->sig != SIG_NONE) break;
			vm_exec_stmt(vm, ienv, ast->u.for_iter.body);
			if (vm->sig == SIG_BREAK && signal_matches_label(vm, NULL)) {
				vm->sig = SIG_NONE; free(vm->sig_label); vm->sig_label = NULL; break;
			}
			if (vm->sig == SIG_CONTINUE && signal_matches_label(vm, NULL)) {
				vm->sig = SIG_NONE; free(vm->sig_label); vm->sig_label = NULL; continue;
			}
			if (vm->sig != SIG_NONE) break;
		}
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_undef();
	}
	case AST_SWITCH: {
		ipo_js_val_t dv = vm_eval_expr(vm, env, ast->u.sw.disc);
		if (vm->sig != SIG_NONE) return ipo_js_undef();
		size_t top = ipo_js_gc_push_root(vm, dv);
		size_t matched = ast->u.sw.len, def_idx = ast->u.sw.len;
		for (size_t i = 0; i < ast->u.sw.len; i++) {
			if (!ast->u.sw.cases[i].test) { def_idx = i; continue; }
			ipo_js_val_t tv = vm_eval_expr(vm, env, ast->u.sw.cases[i].test);
			if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
			if (js_strict_eq(dv, tv)) { matched = i; break; }
		}
		if (matched == ast->u.sw.len) matched = def_idx;
		ipo_js_env_t *senv = vm_alloc_env(vm, env);
		for (size_t i = matched; i < ast->u.sw.len; i++) {
			for (size_t j = 0; j < ast->u.sw.cases[i].body.len; j++) {
				vm_exec_stmt(vm, senv, ast->u.sw.cases[i].body.items[j]);
				if (vm->sig == SIG_BREAK && signal_matches_label(vm, NULL)) {
					vm->sig = SIG_NONE; free(vm->sig_label); vm->sig_label = NULL;
					ipo_js_gc_pop_roots(vm, top);
					return ipo_js_undef();
				}
				if (vm->sig != SIG_NONE) {
					ipo_js_gc_pop_roots(vm, top);
					return ipo_js_undef();
				}
			}
		}
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_undef();
	}
	case AST_LABEL: {
		vm_exec_stmt(vm, env, ast->u.labeled.stmt);
		if ((vm->sig == SIG_BREAK || vm->sig == SIG_CONTINUE) &&
		    vm->sig_label && strcmp(vm->sig_label, ast->u.labeled.label) == 0) {
			vm->sig = SIG_NONE;
			free(vm->sig_label);
			vm->sig_label = NULL;
		}
		return ipo_js_undef();
	}
	case AST_BREAK:
		vm->sig = SIG_BREAK;
		free(vm->sig_label);
		vm->sig_label = ast->u.jump.label ? xstrdup_n(ast->u.jump.label, strlen(ast->u.jump.label)) : NULL;
		return ipo_js_undef();
	case AST_CONTINUE:
		vm->sig = SIG_CONTINUE;
		free(vm->sig_label);
		vm->sig_label = ast->u.jump.label ? xstrdup_n(ast->u.jump.label, strlen(ast->u.jump.label)) : NULL;
		return ipo_js_undef();
	case AST_RETURN: {
		ipo_js_val_t v = ast->u.expr_stmt.expr ? vm_eval_expr(vm, env, ast->u.expr_stmt.expr) : ipo_js_undef();
		if (vm->sig == SIG_NONE) {
			vm->sig = SIG_RETURN;
			vm->sig_val = v;
		}
		return v;
	}
	case AST_THROW: {
		ipo_js_val_t v = vm_eval_expr(vm, env, ast->u.expr_stmt.expr);
		if (vm->sig == SIG_NONE) ipo_js_throw(vm, v);
		return ipo_js_undef();
	}
	case AST_TRY: {
		vm_exec_stmt(vm, env, ast->u.try_s.block);
		if (vm->sig == SIG_THROW && ast->u.try_s.catch_block) {
			ipo_js_val_t ev = vm->exc_val;
			ipo_js_clear_exception(vm);
			ipo_js_env_t *cenv = vm_alloc_env(vm, env);
			if (ast->u.try_s.catch_pat) bind_or_assign_pattern(vm, cenv, ast->u.try_s.catch_pat, ev, 1);
			vm_exec_stmt(vm, cenv, ast->u.try_s.catch_block);
		}
		if (ast->u.try_s.finally_block) {
			js_sig_t saved_sig = vm->sig;
			ipo_js_val_t saved_val = vm->sig_val;
			int saved_has_exc = vm->has_exc;
			ipo_js_val_t saved_exc = vm->exc_val;
			char *saved_lbl = vm->sig_label;
			vm->sig = SIG_NONE; vm->has_exc = 0; vm->sig_label = NULL;
			vm_exec_stmt(vm, env, ast->u.try_s.finally_block);
			if (vm->sig == SIG_NONE) {
				vm->sig = saved_sig;
				vm->sig_val = saved_val;
				vm->has_exc = saved_has_exc;
				vm->exc_val = saved_exc;
				vm->sig_label = saved_lbl;
			} else {
				free(saved_lbl);
			}
		}
		return ipo_js_undef();
	}
	default:
		return ipo_js_undef();
	}
}

/* =========================================================================
 * Promises & Microtasks
 * ========================================================================= */

static void vm_enqueue_microtask(ipo_js_vm_t *vm, ipo_js_val_t fn, ipo_js_val_t arg,
                                 ipo_js_obj_t *target_prom, int is_reject) {
	if (vm->mt_len >= vm->mt_cap) {
		int ncap = vm->mt_cap ? vm->mt_cap * 2 : 16;
		vm->microtasks = (js_microtask_t *)xrealloc(vm->microtasks, (size_t)ncap * sizeof(js_microtask_t));
		vm->mt_cap = ncap;
	}
	vm->microtasks[vm->mt_len++] = (js_microtask_t){
		.fn = fn, .arg = arg, .target_prom = target_prom, .is_reject = is_reject
	};
}

static void promise_settle(ipo_js_vm_t *vm, ipo_js_obj_t *p, int state, ipo_js_val_t val) {
	if (!p || p->kind != JS_OBJ_PROMISE || p->u.prom.state != 0) return;
	if (state == 1 && val.type == JS_T_OBJECT && val.u.obj && val.u.obj->kind == JS_OBJ_PROMISE) {
		ipo_js_obj_t *sub = val.u.obj;
		if (sub->u.prom.state == 1) { promise_settle(vm, p, 1, sub->u.prom.result); return; }
		if (sub->u.prom.state == 2) { promise_settle(vm, p, 2, sub->u.prom.result); return; }
		/* chain sub onto p */
	}
	p->u.prom.state = state;
	p->u.prom.result = val;
	for (int i = 0; i < p->u.prom.cb_len; i++) {
		ipo_js_val_t cb = (state == 1) ? p->u.prom.then_fns[i] : p->u.prom.catch_fns[i];
		ipo_js_obj_t *tp = (p->u.prom.then_promises[i].type == JS_T_OBJECT)
		                       ? p->u.prom.then_promises[i].u.obj : NULL;
		vm_enqueue_microtask(vm, cb, val, tp, state == 2);
	}
}

void ipo_js_promise_resolve(ipo_js_vm_t *vm, ipo_js_obj_t *p, ipo_js_val_t val) {
	promise_settle(vm, p, 1, val);
}

void ipo_js_promise_reject(ipo_js_vm_t *vm, ipo_js_obj_t *p, ipo_js_val_t err) {
	promise_settle(vm, p, 2, err);
}

static void vm_drain_microtasks(ipo_js_vm_t *vm) {
	if (vm->draining_mt) return;
	vm->draining_mt = 1;
	int guard = 0;
	while (vm->mt_head < vm->mt_len && guard++ < 10000) {
		js_microtask_t mt = vm->microtasks[vm->mt_head++];
		size_t top = ipo_js_gc_push_root(vm, mt.fn);
		ipo_js_gc_push_root(vm, mt.arg);
		ipo_js_gc_push_root(vm, ipo_js_obj_val(mt.target_prom));
		if (ipo_js_is_callable(mt.fn)) {
			ipo_js_val_t res = vm_call_obj(vm, mt.fn, ipo_js_undef(), 1, &mt.arg, NULL);
			if (mt.target_prom) {
				if (vm->sig == SIG_THROW) {
					ipo_js_val_t ev = vm->exc_val;
					ipo_js_clear_exception(vm);
					promise_settle(vm, mt.target_prom, 2, ev);
				} else {
					promise_settle(vm, mt.target_prom, 1, res);
				}
			}
		} else if (mt.target_prom) {
			promise_settle(vm, mt.target_prom, mt.is_reject ? 2 : 1, mt.arg);
		}
		ipo_js_gc_pop_roots(vm, top);
	}
	vm->mt_head = 0;
	vm->mt_len = 0;
	vm->draining_mt = 0;
}

/* =========================================================================
 * JSON.parse / JSON.stringify
 * ========================================================================= */

typedef struct {
	const char *p;
	const char *end;
	ipo_js_vm_t *vm;
	int depth;
} json_parser_t;

static void json_skip_ws(json_parser_t *jp) {
	while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) jp->p++;
}

static ipo_js_val_t json_parse_val(json_parser_t *jp, int *ok);

static ipo_js_str_t *json_parse_str(json_parser_t *jp, int *ok) {
	if (jp->p >= jp->end || *jp->p != '"') { *ok = 0; return NULL; }
	jp->p++;
	js_buf_t b;
	jbuf_init(&b);
	while (jp->p < jp->end && *jp->p != '"') {
		char ch = *jp->p++;
		if ((unsigned char)ch < 0x20) { jbuf_free(&b); *ok = 0; return NULL; }
		if (ch == '\\') {
			if (jp->p >= jp->end) { jbuf_free(&b); *ok = 0; return NULL; }
			char e = *jp->p++;
			switch (e) {
			case '"': case '\\': case '/': jbuf_append_ch(&b, e); break;
			case 'b': jbuf_append_ch(&b, '\b'); break;
			case 'f': jbuf_append_ch(&b, '\f'); break;
			case 'n': jbuf_append_ch(&b, '\n'); break;
			case 'r': jbuf_append_ch(&b, '\r'); break;
			case 't': jbuf_append_ch(&b, '\t'); break;
			case 'u':
				if (jp->p + 3 < jp->end) {
					int a = re_hex_digit(jp->p[0]), bb = re_hex_digit(jp->p[1]),
					    cc = re_hex_digit(jp->p[2]), dd = re_hex_digit(jp->p[3]);
					if (a >= 0 && bb >= 0 && cc >= 0 && dd >= 0) {
						uint32_t cp = (uint32_t)((a << 12) | (bb << 8) | (cc << 4) | dd);
						jp->p += 4;
						jbuf_append_utf8(&b, cp);
						break;
					}
				}
				jbuf_free(&b); *ok = 0; return NULL;
			default:
				jbuf_free(&b); *ok = 0; return NULL;
			}
		} else {
			jbuf_append_ch(&b, ch);
		}
	}
	if (jp->p >= jp->end || *jp->p != '"') { jbuf_free(&b); *ok = 0; return NULL; }
	jp->p++;
	ipo_js_str_t *st = vm_alloc_str(jp->vm, b.data ? b.data : "", b.len);
	jbuf_free(&b);
	return st;
}

static ipo_js_val_t json_parse_val(json_parser_t *jp, int *ok) {
	if (++jp->depth > 256) { *ok = 0; return ipo_js_undef(); }
	json_skip_ws(jp);
	if (jp->p >= jp->end) { *ok = 0; jp->depth--; return ipo_js_undef(); }
	char c = *jp->p;
	ipo_js_val_t res = ipo_js_undef();
	if (c == '"') {
		ipo_js_str_t *s = json_parse_str(jp, ok);
		res = (ipo_js_val_t){.type = JS_T_STRING, .u.str = s};
	} else if (c == '{') {
		jp->p++;
		ipo_js_obj_t *o = ipo_js_new_object(jp->vm, NULL);
		size_t top = ipo_js_gc_push_root(jp->vm, ipo_js_obj_val(o));
		json_skip_ws(jp);
		if (jp->p < jp->end && *jp->p == '}') { jp->p++; }
		else {
			for (;;) {
				json_skip_ws(jp);
				ipo_js_str_t *k = json_parse_str(jp, ok);
				if (!*ok) break;
				size_t ktop = ipo_js_gc_push_root(jp->vm, (ipo_js_val_t){.type = JS_T_STRING, .u.str = k});
				json_skip_ws(jp);
				if (jp->p >= jp->end || *jp->p != ':') { *ok = 0; ipo_js_gc_pop_roots(jp->vm, ktop); break; }
				jp->p++;
				ipo_js_val_t v = json_parse_val(jp, ok);
				if (!*ok) { ipo_js_gc_pop_roots(jp->vm, ktop); break; }
				ipo_js_set_prop(jp->vm, o, k->data, v);
				ipo_js_gc_pop_roots(jp->vm, ktop);
				json_skip_ws(jp);
				if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
				if (jp->p < jp->end && *jp->p == '}') { jp->p++; break; }
				*ok = 0; break;
			}
		}
		ipo_js_gc_pop_roots(jp->vm, top);
		res = ipo_js_obj_val(o);
	} else if (c == '[') {
		jp->p++;
		ipo_js_obj_t *a = ipo_js_new_array(jp->vm, 0);
		size_t top = ipo_js_gc_push_root(jp->vm, ipo_js_obj_val(a));
		json_skip_ws(jp);
		if (jp->p < jp->end && *jp->p == ']') { jp->p++; }
		else {
			for (;;) {
				ipo_js_val_t v = json_parse_val(jp, ok);
				if (!*ok) break;
				ipo_js_arr_push(jp->vm, a, v);
				json_skip_ws(jp);
				if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
				if (jp->p < jp->end && *jp->p == ']') { jp->p++; break; }
				*ok = 0; break;
			}
		}
		ipo_js_gc_pop_roots(jp->vm, top);
		res = ipo_js_obj_val(a);
	} else if (c == 't' && jp->p + 4 <= jp->end && memcmp(jp->p, "true", 4) == 0) {
		jp->p += 4; res = ipo_js_bool(1);
	} else if (c == 'f' && jp->p + 5 <= jp->end && memcmp(jp->p, "false", 5) == 0) {
		jp->p += 5; res = ipo_js_bool(0);
	} else if (c == 'n' && jp->p + 4 <= jp->end && memcmp(jp->p, "null", 4) == 0) {
		jp->p += 4; res = ipo_js_null();
	} else if (c == '-' || isdigit((unsigned char)c)) {
		char *endp = NULL;
		double d = strtod(jp->p, &endp);
		if (!endp || endp == jp->p || endp > jp->end) *ok = 0;
		else { jp->p = endp; res = ipo_js_num(d); }
	} else {
		*ok = 0;
	}
	jp->depth--;
	return res;
}

int ipo_js_json_parse(ipo_js_vm_t *vm, const char *s, size_t len, ipo_js_val_t *out) {
	json_parser_t jp = { s ? s : "", (s ? s : "") + len, vm, 0 };
	int ok = 1;
	ipo_js_val_t v = json_parse_val(&jp, &ok);
	if (ok) {
		json_skip_ws(&jp);
		if (jp.p != jp.end) ok = 0;
	}
	if (!ok) {
		ipo_js_throw_msg(vm, "SyntaxError", "Invalid JSON string");
		*out = ipo_js_undef();
		return -1;
	}
	*out = v;
	return 0;
}

static void json_escape_str(js_buf_t *b, const char *s, size_t len) {
	jbuf_append_ch(b, '"');
	for (size_t i = 0; i < len; i++) {
		unsigned char ch = (unsigned char)s[i];
		switch (ch) {
		case '"':  jbuf_append_cstr(b, "\\\""); break;
		case '\\': jbuf_append_cstr(b, "\\\\"); break;
		case '\b': jbuf_append_cstr(b, "\\b"); break;
		case '\f': jbuf_append_cstr(b, "\\f"); break;
		case '\n': jbuf_append_cstr(b, "\\n"); break;
		case '\r': jbuf_append_cstr(b, "\\r"); break;
		case '\t': jbuf_append_cstr(b, "\\t"); break;
		default:
			if (ch < 0x20) {
				char esc[8];
				snprintf(esc, sizeof(esc), "\\u%04x", ch);
				jbuf_append_cstr(b, esc);
			} else {
				jbuf_append_ch(b, (char)ch);
			}
		}
	}
	jbuf_append_ch(b, '"');
}

static void json_stringify_rec(ipo_js_vm_t *vm, js_buf_t *b, ipo_js_val_t v, int indent, int depth,
                               ipo_js_obj_t **seen, int seen_n) {
	if (depth > 64) { jbuf_append_cstr(b, "null"); return; }
	switch (v.type) {
	case JS_T_NULL:      jbuf_append_cstr(b, "null"); return;
	case JS_T_UNDEFINED: jbuf_append_cstr(b, "null"); return;
	case JS_T_BOOL:      jbuf_append_cstr(b, v.u.b ? "true" : "false"); return;
	case JS_T_NUMBER:
		if (!isfinite(v.u.num)) jbuf_append_cstr(b, "null");
		else {
			char nb[64];
			fmt_js_number(v.u.num, nb, sizeof(nb));
			jbuf_append_cstr(b, nb);
		}
		return;
	case JS_T_STRING:
		json_escape_str(b, v.u.str->data, v.u.str->len);
		return;
	case JS_T_OBJECT: {
		ipo_js_obj_t *o = v.u.obj;
		if (!o || ipo_js_is_callable(v)) { jbuf_append_cstr(b, "null"); return; }
		for (int i = 0; i < seen_n; i++) {
			if (seen[i] == o) { jbuf_append_cstr(b, "null"); return; }
		}
		ipo_js_obj_t *nseen[64];
		if (seen_n < 64) {
			if (seen_n > 0) memcpy(nseen, seen, (size_t)seen_n * sizeof(ipo_js_obj_t *));
			nseen[seen_n] = o;
		}
		if (o->kind == JS_OBJ_ARRAY) {
			jbuf_append_ch(b, '[');
			for (size_t i = 0; i < o->u.arr.len; i++) {
				if (i > 0) jbuf_append_ch(b, ',');
				if (indent > 0) {
					jbuf_append_ch(b, '\n');
					for (int k = 0; k < (depth + 1) * indent; k++) jbuf_append_ch(b, ' ');
				}
				json_stringify_rec(vm, b, o->u.arr.items[i], indent, depth + 1, nseen, seen_n + 1);
			}
			if (o->u.arr.len > 0 && indent > 0) {
				jbuf_append_ch(b, '\n');
				for (int k = 0; k < depth * indent; k++) jbuf_append_ch(b, ' ');
			}
			jbuf_append_ch(b, ']');
			return;
		}
		jbuf_append_ch(b, '{');
		int first = 1;
		for (size_t i = 0; i < o->prop_len; i++) {
			ipo_js_prop_t *pr = &o->props[i];
			if (!(pr->flags & 0x01)) continue;
			ipo_js_val_t pv = ipo_js_undef();
			ipo_js_get_prop(vm, o, pr->key->data, &pv);
			if (pv.type == JS_T_UNDEFINED || ipo_js_is_callable(pv)) continue;
			if (!first) jbuf_append_ch(b, ',');
			first = 0;
			if (indent > 0) {
				jbuf_append_ch(b, '\n');
				for (int k = 0; k < (depth + 1) * indent; k++) jbuf_append_ch(b, ' ');
			}
			json_escape_str(b, pr->key->data, pr->key->len);
			jbuf_append_cstr(b, indent > 0 ? ": " : ":");
			json_stringify_rec(vm, b, pv, indent, depth + 1, nseen, seen_n + 1);
		}
		if (!first && indent > 0) {
			jbuf_append_ch(b, '\n');
			for (int k = 0; k < depth * indent; k++) jbuf_append_ch(b, ' ');
		}
		jbuf_append_ch(b, '}');
		return;
	}
	}
}

ipo_js_str_t *ipo_js_json_stringify(ipo_js_vm_t *vm, ipo_js_val_t val, int indent) {
	if (val.type == JS_T_UNDEFINED || ipo_js_is_callable(val)) return NULL;
	js_buf_t b;
	jbuf_init(&b);
	json_stringify_rec(vm, &b, val, indent, 0, NULL, 0);
	ipo_js_str_t *st = vm_alloc_str(vm, b.data ? b.data : "", b.len);
	jbuf_free(&b);
	return st;
}

/* =========================================================================
 * Built-in Natives (Object, Array, String, Math, Date, JSON, RegExp, ...)
 * ========================================================================= */

static ipo_js_val_t js_nat_console_log(ipo_js_vm_t *vm, ipo_js_val_t this_val,
                                       int argc, const ipo_js_val_t *argv, void *ud) {
	(void)this_val;
	int level = (int)(intptr_t)ud;
	js_buf_t b;
	jbuf_init(&b);
	for (int i = 0; i < argc; i++) {
		if (i > 0) jbuf_append_ch(&b, ' ');
		if (argv[i].type == JS_T_OBJECT && argv[i].u.obj && argv[i].u.obj->kind != JS_OBJ_FUNCTION) {
			ipo_js_str_t *js = ipo_js_json_stringify(vm, argv[i], 0);
			if (js) { jbuf_append(&b, js->data, js->len); continue; }
		}
		size_t slen = 0;
		const char *s = ipo_js_cstr(vm, argv[i], &slen);
		jbuf_append(&b, s, slen);
	}
	if (vm->console_hook) vm->console_hook(vm->host_ctx, level, b.data ? b.data : "");
	jbuf_free(&b);
	return ipo_js_undef();
}

static ipo_js_val_t js_nat_parse_int(ipo_js_vm_t *vm, ipo_js_val_t tv,
                                     int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_num(NAN);
	const char *s = ipo_js_cstr(vm, argv[0], NULL);
	while (*s && isspace((unsigned char)*s)) s++;
	int radix = (argc >= 2) ? ipo_js_to_i32(vm, argv[1]) : 0;
	if (radix != 0 && (radix < 2 || radix > 36)) return ipo_js_num(NAN);
	int neg = 0;
	if (*s == '-') { neg = 1; s++; }
	else if (*s == '+') s++;
	if (radix == 0) {
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { radix = 16; s += 2; }
		else radix = 10;
	} else if (radix == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	double acc = 0.0;
	int any = 0;
	while (*s) {
		unsigned char c = (unsigned char)*s;
		int d = -1;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
		if (d < 0 || d >= radix) break;
		any = 1;
		acc = acc * radix + d;
		s++;
	}
	if (!any) return ipo_js_num(NAN);
	return ipo_js_num(neg ? -acc : acc);
}

static ipo_js_val_t js_nat_parse_float(ipo_js_vm_t *vm, ipo_js_val_t tv,
                                       int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_num(NAN);
	const char *s = ipo_js_cstr(vm, argv[0], NULL);
	while (*s && isspace((unsigned char)*s)) s++;
	if (!*s) return ipo_js_num(NAN);
	char *endp = NULL;
	double d = strtod(s, &endp);
	if (!endp || endp == s) return ipo_js_num(NAN);
	return ipo_js_num(d);
}

static ipo_js_val_t js_nat_is_nan(ipo_js_vm_t *vm, ipo_js_val_t tv,
                                  int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_bool(1);
	return ipo_js_bool(isnan(ipo_js_to_num(vm, argv[0])));
}

static ipo_js_val_t js_nat_is_finite(ipo_js_vm_t *vm, ipo_js_val_t tv,
                                     int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_bool(0);
	return ipo_js_bool(isfinite(ipo_js_to_num(vm, argv[0])));
}

static int is_unreserved_uri(unsigned char c, int full_uri) {
	if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '!' ||
	    c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') return 1;
	if (full_uri && strchr(";/?:@&=+$,#", c) != NULL) return 1;
	return 0;
}

static ipo_js_val_t js_nat_encode_uri(ipo_js_vm_t *vm, ipo_js_val_t tv,
                                      int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int full = (int)(intptr_t)ud;
	if (argc < 1) return ipo_js_str_cstr(vm, "undefined");
	size_t len = 0;
	const char *s = ipo_js_cstr(vm, argv[0], &len);
	js_buf_t b; jbuf_init(&b);
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (is_unreserved_uri(c, full)) jbuf_append_ch(&b, (char)c);
		else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", c); jbuf_append(&b, buf, 3); }
	}
	ipo_js_val_t r = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
	jbuf_free(&b);
	return r;
}

static ipo_js_val_t js_nat_decode_uri(ipo_js_vm_t *vm, ipo_js_val_t tv,
                                      int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_str_cstr(vm, "undefined");
	size_t len = 0;
	const char *s = ipo_js_cstr(vm, argv[0], &len);
	js_buf_t b; jbuf_init(&b);
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '%' && i + 2 < len) {
			int hi = re_hex_digit(s[i + 1]), lo = re_hex_digit(s[i + 2]);
			if (hi >= 0 && lo >= 0) { jbuf_append_ch(&b, (char)((hi << 4) | lo)); i += 2; continue; }
		}
		jbuf_append_ch(&b, s[i]);
	}
	ipo_js_val_t r = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
	jbuf_free(&b);
	return r;
}

static ipo_js_val_t js_nat_eval(ipo_js_vm_t *vm, ipo_js_val_t tv,
                                int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_undef();
	if (argv[0].type != JS_T_STRING || !argv[0].u.str) return argv[0];
	ipo_js_ast_t *root = NULL;
	js_ast_pool_t *pool = parse_program(vm, argv[0].u.str->data, argv[0].u.str->len, "<eval>", &root);
	if (!pool) return ipo_js_undef();
	vm->ast_pools = (js_ast_pool_t **)xrealloc(vm->ast_pools, (vm->ast_pool_len + 1) * sizeof(js_ast_pool_t *));
	vm->ast_pools[vm->ast_pool_len++] = pool;
	ipo_js_env_t *target_env = (vm->frame_len > 0) ? vm->frames[vm->frame_len - 1].env : vm->global_env;
	hoist_block(vm, target_env, target_env, root, 1);
	return vm_exec_stmt(vm, target_env, root);
}

/* Function.prototype methods */
static ipo_js_val_t js_fn_call_apply(ipo_js_vm_t *vm, ipo_js_val_t tv,
                                     int argc, const ipo_js_val_t *argv, void *ud) {
	int is_apply = (int)(intptr_t)ud;
	if (!ipo_js_is_callable(tv)) {
		ipo_js_throw_msg(vm, "TypeError", "Function required");
		return ipo_js_undef();
	}
	ipo_js_val_t this_arg = (argc >= 1) ? argv[0] : ipo_js_undef();
	if (!is_apply) {
		return vm_call_obj(vm, tv, this_arg, argc > 1 ? argc - 1 : 0, argc > 1 ? argv + 1 : NULL, NULL);
	}
	if (argc < 2 || argv[1].type != JS_T_OBJECT || !argv[1].u.obj || argv[1].u.obj->kind != JS_OBJ_ARRAY) {
		return vm_call_obj(vm, tv, this_arg, 0, NULL, NULL);
	}
	ipo_js_obj_t *a = argv[1].u.obj;
	return vm_call_obj(vm, tv, this_arg, (int)a->u.arr.len, a->u.arr.items, NULL);
}

static ipo_js_val_t js_fn_bind(ipo_js_vm_t *vm, ipo_js_val_t tv,
                               int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (!ipo_js_is_callable(tv)) return ipo_js_undef();
	ipo_js_obj_t *src = tv.u.obj;
	ipo_js_obj_t *bf = vm_alloc_obj(vm, src->kind, vm->fn_proto);
	*bf = *src;
	bf->props = NULL; bf->prop_len = 0; bf->prop_cap = 0; bf->pin_count = 0;
	bf->gc_next = vm->gc_objs; vm->gc_objs = bf;
	ipo_js_val_t bthis = argc >= 1 ? argv[0] : ipo_js_undef();
	int ba_cnt = argc > 1 ? argc - 1 : 0;
	ipo_js_val_t *ba = NULL;
	if (ba_cnt > 0) {
		ba = (ipo_js_val_t *)xmalloc((size_t)ba_cnt * sizeof(ipo_js_val_t));
		memcpy(ba, argv + 1, (size_t)ba_cnt * sizeof(ipo_js_val_t));
	}
	if (bf->kind == JS_OBJ_FUNCTION) {
		bf->u.fn.is_bound = 1; bf->u.fn.bound_this = bthis;
		bf->u.fn.bound_args = ba; bf->u.fn.bound_argc = ba_cnt;
	} else {
		bf->u.nat.is_bound = 1; bf->u.nat.bound_this = bthis;
		bf->u.nat.bound_args = ba; bf->u.nat.bound_argc = ba_cnt;
	}
	return ipo_js_obj_val(bf);
}

/* Object statics & prototype */
static ipo_js_val_t js_obj_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc >= 1 && argv[0].type == JS_T_OBJECT && argv[0].u.obj) return argv[0];
	return ipo_js_obj_val(ipo_js_new_object(vm, vm->obj_proto));
}

static ipo_js_val_t js_obj_keys_vals_entries(ipo_js_vm_t *vm, ipo_js_val_t tv,
                                            int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int mode = (int)(intptr_t)ud; /* 0=keys, 1=values, 2=entries */
	ipo_js_obj_t *arr = ipo_js_new_array(vm, 0);
	if (argc < 1 || argv[0].type != JS_T_OBJECT || !argv[0].u.obj) return ipo_js_obj_val(arr);
	ipo_js_obj_t *o = argv[0].u.obj;
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(arr));
	if (o->kind == JS_OBJ_ARRAY) {
		for (size_t i = 0; i < o->u.arr.len; i++) {
			char nb[24]; snprintf(nb, sizeof(nb), "%lu", (unsigned long)i);
			if (mode == 0) ipo_js_arr_push(vm, arr, ipo_js_str_cstr(vm, nb));
			else if (mode == 1) ipo_js_arr_push(vm, arr, o->u.arr.items[i]);
			else {
				ipo_js_obj_t *pair = ipo_js_new_array(vm, 2);
				pair->u.arr.items[0] = ipo_js_str_cstr(vm, nb);
				pair->u.arr.items[1] = o->u.arr.items[i];
				ipo_js_arr_push(vm, arr, ipo_js_obj_val(pair));
			}
		}
	}
	for (size_t i = 0; i < o->prop_len; i++) {
		if (!(o->props[i].flags & 0x01)) continue;
		ipo_js_val_t pv = ipo_js_undef();
		if (mode != 0) ipo_js_get_prop(vm, o, o->props[i].key->data, &pv);
		if (mode == 0) ipo_js_arr_push(vm, arr, (ipo_js_val_t){.type = JS_T_STRING, .u.str = o->props[i].key});
		else if (mode == 1) ipo_js_arr_push(vm, arr, pv);
		else {
			ipo_js_obj_t *pair = ipo_js_new_array(vm, 2);
			pair->u.arr.items[0] = (ipo_js_val_t){.type = JS_T_STRING, .u.str = o->props[i].key};
			pair->u.arr.items[1] = pv;
			ipo_js_arr_push(vm, arr, ipo_js_obj_val(pair));
		}
	}
	ipo_js_gc_pop_roots(vm, top);
	return ipo_js_obj_val(arr);
}

static ipo_js_val_t js_obj_assign(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1 || argv[0].type != JS_T_OBJECT || !argv[0].u.obj) return ipo_js_undef();
	ipo_js_obj_t *dst = argv[0].u.obj;
	for (int i = 1; i < argc; i++) {
		if (argv[i].type != JS_T_OBJECT || !argv[i].u.obj) continue;
		ipo_js_obj_t *src = argv[i].u.obj;
		for (size_t j = 0; j < src->prop_len; j++) {
			if (src->props[j].flags & 0x01) {
				ipo_js_val_t pv = ipo_js_undef();
				ipo_js_get_prop(vm, src, src->props[j].key->data, &pv);
				ipo_js_set_prop(vm, dst, src->props[j].key->data, pv);
			}
		}
	}
	return argv[0];
}

static ipo_js_val_t js_obj_create(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	ipo_js_obj_t *proto = (argc >= 1 && argv[0].type == JS_T_OBJECT) ? argv[0].u.obj : NULL;
	return ipo_js_obj_val(vm_alloc_obj(vm, JS_OBJ_PLAIN, proto));
}

static ipo_js_val_t js_obj_define_prop(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 3 || argv[0].type != JS_T_OBJECT || !argv[0].u.obj ||
	    argv[2].type != JS_T_OBJECT || !argv[2].u.obj) return argc >= 1 ? argv[0] : ipo_js_undef();
	const char *k = ipo_js_cstr(vm, argv[1], NULL);
	ipo_js_obj_t *desc = argv[2].u.obj;
	ipo_js_val_t g = ipo_js_undef(), s = ipo_js_undef(), v = ipo_js_undef();
	int hg = ipo_js_has_own_prop(desc, "get") && ipo_js_get_prop(vm, desc, "get", &g) && g.type == JS_T_OBJECT;
	int hs = ipo_js_has_own_prop(desc, "set") && ipo_js_get_prop(vm, desc, "set", &s) && s.type == JS_T_OBJECT;
	if (hg || hs) {
		obj_define_accessor(vm, argv[0].u.obj, k, hg ? g.u.obj : NULL, hs ? s.u.obj : NULL);
	} else if (ipo_js_get_prop(vm, desc, "value", &v)) {
		ipo_js_set_prop(vm, argv[0].u.obj, k, v);
	}
	return argv[0];
}

static ipo_js_val_t js_obj_has_own(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || argc < 1) return ipo_js_bool(0);
	return ipo_js_bool(ipo_js_has_own_prop(tv.u.obj, ipo_js_cstr(vm, argv[0], NULL)));
}

static ipo_js_val_t js_obj_to_str(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)argc; (void)argv; (void)ud;
	if (tv.type == JS_T_OBJECT && tv.u.obj) {
		if (tv.u.obj->kind == JS_OBJ_ARRAY) {
			js_buf_t b; jbuf_init(&b);
			for (size_t i = 0; i < tv.u.obj->u.arr.len; i++) {
				if (i > 0) jbuf_append_ch(&b, ',');
				size_t slen = 0;
				const char *s = ipo_js_cstr(vm, tv.u.obj->u.arr.items[i], &slen);
				jbuf_append(&b, s, slen);
			}
			ipo_js_val_t r = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
			jbuf_free(&b);
			return r;
		}
		if (tv.u.obj->kind == JS_OBJ_ERROR) {
			ipo_js_val_t n = ipo_js_undef(), m = ipo_js_undef();
			ipo_js_get_prop(vm, tv.u.obj, "name", &n);
			ipo_js_get_prop(vm, tv.u.obj, "message", &m);
			char buf[256];
			snprintf(buf, sizeof(buf), "%s: %s", ipo_js_cstr(vm, n, NULL), ipo_js_cstr(vm, m, NULL));
			return ipo_js_str_cstr(vm, buf);
		}
	}
	return ipo_js_str_cstr(vm, "[object Object]");
}

/* Array statics & methods */
static ipo_js_val_t js_arr_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc == 1 && argv[0].type == JS_T_NUMBER) {
		size_t n = argv[0].u.num >= 0 ? (size_t)argv[0].u.num : 0;
		return ipo_js_obj_val(ipo_js_new_array(vm, n));
	}
	ipo_js_obj_t *a = ipo_js_new_array(vm, (size_t)argc);
	for (int i = 0; i < argc; i++) a->u.arr.items[i] = argv[i];
	return ipo_js_obj_val(a);
}

static ipo_js_val_t js_arr_is_array(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)vm; (void)tv; (void)ud;
	return ipo_js_bool(argc >= 1 && argv[0].type == JS_T_OBJECT && argv[0].u.obj && argv[0].u.obj->kind == JS_OBJ_ARRAY);
}

static ipo_js_val_t js_arr_from(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	ipo_js_obj_t *res = ipo_js_new_array(vm, 0);
	if (argc < 1) return ipo_js_obj_val(res);
	ipo_js_val_t map_fn = (argc >= 2 && ipo_js_is_callable(argv[1])) ? argv[1] : ipo_js_undef();
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(res));
	if (argv[0].type == JS_T_STRING && argv[0].u.str) {
		for (size_t i = 0; i < argv[0].u.str->len; i++) {
			ipo_js_val_t item = ipo_js_str_new(vm, &argv[0].u.str->data[i], 1);
			if (!ipo_js_is_callable(map_fn)) ipo_js_arr_push(vm, res, item);
			else {
				ipo_js_val_t args[2] = { item, ipo_js_num((double)i) };
				ipo_js_arr_push(vm, res, vm_call_obj(vm, map_fn, ipo_js_undef(), 2, args, NULL));
			}
		}
	} else if (argv[0].type == JS_T_OBJECT && argv[0].u.obj) {
		ipo_js_val_t lv = ipo_js_undef();
		ipo_js_get_prop(vm, argv[0].u.obj, "length", &lv);
		size_t n = lv.type == JS_T_NUMBER && lv.u.num > 0 ? (size_t)lv.u.num : 0;
		for (size_t i = 0; i < n; i++) {
			char nb[24]; snprintf(nb, sizeof(nb), "%lu", (unsigned long)i);
			ipo_js_val_t item = ipo_js_undef();
			ipo_js_get_prop(vm, argv[0].u.obj, nb, &item);
			if (!ipo_js_is_callable(map_fn)) ipo_js_arr_push(vm, res, item);
			else {
				ipo_js_val_t args[2] = { item, ipo_js_num((double)i) };
				ipo_js_arr_push(vm, res, vm_call_obj(vm, map_fn, ipo_js_undef(), 2, args, NULL));
			}
		}
	}
	ipo_js_gc_pop_roots(vm, top);
	return ipo_js_obj_val(res);
}

static ipo_js_val_t js_arr_push(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY) return ipo_js_num(0);
	for (int i = 0; i < argc; i++) ipo_js_arr_push(vm, tv.u.obj, argv[i]);
	return ipo_js_num((double)tv.u.obj->u.arr.len);
}

static ipo_js_val_t js_arr_pop(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)vm; (void)argc; (void)argv; (void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY || tv.u.obj->u.arr.len == 0)
		return ipo_js_undef();
	return tv.u.obj->u.arr.items[--tv.u.obj->u.arr.len];
}

static ipo_js_val_t js_arr_shift(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)vm; (void)argc; (void)argv; (void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY || tv.u.obj->u.arr.len == 0)
		return ipo_js_undef();
	ipo_js_obj_t *a = tv.u.obj;
	ipo_js_val_t first = a->u.arr.items[0];
	memmove(a->u.arr.items, a->u.arr.items + 1, (a->u.arr.len - 1) * sizeof(ipo_js_val_t));
	a->u.arr.len--;
	return first;
}

static ipo_js_val_t js_arr_unshift(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY) return ipo_js_num(0);
	ipo_js_obj_t *a = tv.u.obj;
	size_t old_len = a->u.arr.len;
	arr_resize(vm, a, old_len + (size_t)argc);
	if (old_len > 0 && argc > 0)
		memmove(a->u.arr.items + argc, a->u.arr.items, old_len * sizeof(ipo_js_val_t));
	for (int i = 0; i < argc; i++) a->u.arr.items[i] = argv[i];
	return ipo_js_num((double)a->u.arr.len);
}

static ipo_js_val_t js_arr_join(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY) return ipo_js_str_cstr(vm, "");
	const char *sep = ",";
	size_t seplen = 1;
	if (argc >= 1 && argv[0].type != JS_T_UNDEFINED) sep = ipo_js_cstr(vm, argv[0], &seplen);
	js_buf_t b; jbuf_init(&b);
	ipo_js_obj_t *a = tv.u.obj;
	for (size_t i = 0; i < a->u.arr.len; i++) {
		if (i > 0) jbuf_append(&b, sep, seplen);
		ipo_js_val_t it = a->u.arr.items[i];
		if (it.type == JS_T_UNDEFINED || it.type == JS_T_NULL) continue;
		size_t slen = 0;
		const char *s = ipo_js_cstr(vm, it, &slen);
		jbuf_append(&b, s, slen);
	}
	ipo_js_val_t r = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
	jbuf_free(&b);
	return r;
}

static size_t norm_idx(int32_t idx, size_t len) {
	if (idx < 0) {
		int64_t p = (int64_t)len + idx;
		return p < 0 ? 0 : (size_t)p;
	}
	return (size_t)idx > len ? len : (size_t)idx;
}

static ipo_js_val_t js_arr_slice(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY)
		return ipo_js_obj_val(ipo_js_new_array(vm, 0));
	ipo_js_obj_t *a = tv.u.obj;
	size_t s = (argc >= 1 && argv[0].type != JS_T_UNDEFINED) ? norm_idx(ipo_js_to_i32(vm, argv[0]), a->u.arr.len) : 0;
	size_t e = (argc >= 2 && argv[1].type != JS_T_UNDEFINED) ? norm_idx(ipo_js_to_i32(vm, argv[1]), a->u.arr.len) : a->u.arr.len;
	if (e < s) e = s;
	ipo_js_obj_t *out = ipo_js_new_array(vm, e - s);
	for (size_t i = s; i < e; i++) out->u.arr.items[i - s] = a->u.arr.items[i];
	return ipo_js_obj_val(out);
}

static ipo_js_val_t js_arr_splice(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY)
		return ipo_js_obj_val(ipo_js_new_array(vm, 0));
	ipo_js_obj_t *a = tv.u.obj;
	size_t start = (argc >= 1) ? norm_idx(ipo_js_to_i32(vm, argv[0]), a->u.arr.len) : a->u.arr.len;
	int32_t del = (argc >= 2) ? ipo_js_to_i32(vm, argv[1]) : (int32_t)(a->u.arr.len - start);
	if (del < 0) del = 0;
	if ((size_t)del > a->u.arr.len - start) del = (int32_t)(a->u.arr.len - start);
	ipo_js_obj_t *removed = ipo_js_new_array(vm, (size_t)del);
	for (int32_t i = 0; i < del; i++) removed->u.arr.items[i] = a->u.arr.items[start + (size_t)i];
	int ins = argc > 2 ? argc - 2 : 0;
	size_t old_len = a->u.arr.len;
	size_t new_len = old_len - (size_t)del + (size_t)ins;
	if (new_len > old_len) arr_resize(vm, a, new_len);
	size_t tail = old_len - start - (size_t)del;
	if (tail > 0) memmove(a->u.arr.items + start + ins, a->u.arr.items + start + del, tail * sizeof(ipo_js_val_t));
	for (int i = 0; i < ins; i++) a->u.arr.items[start + (size_t)i] = argv[2 + i];
	a->u.arr.len = new_len;
	return ipo_js_obj_val(removed);
}

static ipo_js_val_t js_arr_concat(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	ipo_js_obj_t *out = ipo_js_new_array(vm, 0);
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(out));
	if (tv.type == JS_T_OBJECT && tv.u.obj && tv.u.obj->kind == JS_OBJ_ARRAY) {
		for (size_t i = 0; i < tv.u.obj->u.arr.len; i++) ipo_js_arr_push(vm, out, tv.u.obj->u.arr.items[i]);
	}
	for (int i = 0; i < argc; i++) {
		if (argv[i].type == JS_T_OBJECT && argv[i].u.obj && argv[i].u.obj->kind == JS_OBJ_ARRAY) {
			for (size_t j = 0; j < argv[i].u.obj->u.arr.len; j++)
				ipo_js_arr_push(vm, out, argv[i].u.obj->u.arr.items[j]);
		} else {
			ipo_js_arr_push(vm, out, argv[i]);
		}
	}
	ipo_js_gc_pop_roots(vm, top);
	return ipo_js_obj_val(out);
}

static ipo_js_val_t js_arr_reverse(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)vm; (void)argc; (void)argv; (void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY) return tv;
	ipo_js_obj_t *a = tv.u.obj;
	for (size_t i = 0, j = a->u.arr.len; i + 1 < j; i++, j--) {
		ipo_js_val_t tmp = a->u.arr.items[i];
		a->u.arr.items[i] = a->u.arr.items[j - 1];
		a->u.arr.items[j - 1] = tmp;
	}
	return tv;
}

static ipo_js_val_t js_arr_sort(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY) return tv;
	ipo_js_obj_t *a = tv.u.obj;
	ipo_js_val_t cmp = (argc >= 1 && ipo_js_is_callable(argv[0])) ? argv[0] : ipo_js_undef();
	for (size_t i = 1; i < a->u.arr.len; i++) {
		ipo_js_val_t key = a->u.arr.items[i];
		size_t j = i;
		while (j > 0) {
			int greater = 0;
			if (ipo_js_is_callable(cmp)) {
				ipo_js_val_t args[2] = { a->u.arr.items[j - 1], key };
				ipo_js_val_t r = vm_call_obj(vm, cmp, ipo_js_undef(), 2, args, NULL);
				if (vm->sig != SIG_NONE) return tv;
				greater = (ipo_js_to_num(vm, r) > 0.0);
			} else {
				const char *sa = ipo_js_cstr(vm, a->u.arr.items[j - 1], NULL);
				size_t top = ipo_js_gc_push_root(vm, ipo_js_str_cstr(vm, sa));
				const char *sb = ipo_js_cstr(vm, key, NULL);
				greater = (strcmp(sa, sb) > 0);
				ipo_js_gc_pop_roots(vm, top);
			}
			if (!greater) break;
			a->u.arr.items[j] = a->u.arr.items[j - 1];
			j--;
		}
		a->u.arr.items[j] = key;
	}
	return tv;
}

static ipo_js_val_t js_arr_index_of(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int mode = (int)(intptr_t)ud; /* 0=indexOf, 1=lastIndexOf, 2=includes */
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY || argc < 1)
		return mode == 2 ? ipo_js_bool(0) : ipo_js_num(-1);
	ipo_js_obj_t *a = tv.u.obj;
	if (mode == 1) {
		for (size_t i = a->u.arr.len; i > 0; i--) {
			if (js_strict_eq(a->u.arr.items[i - 1], argv[0])) return ipo_js_num((double)(i - 1));
		}
		return ipo_js_num(-1);
	}
	size_t from = (argc >= 2) ? norm_idx(ipo_js_to_i32(vm, argv[1]), a->u.arr.len) : 0;
	for (size_t i = from; i < a->u.arr.len; i++) {
		if (js_strict_eq(a->u.arr.items[i], argv[0]) ||
		    (mode == 2 && a->u.arr.items[i].type == JS_T_NUMBER && argv[0].type == JS_T_NUMBER &&
		     isnan(a->u.arr.items[i].u.num) && isnan(argv[0].u.num))) {
			return mode == 2 ? ipo_js_bool(1) : ipo_js_num((double)i);
		}
	}
	return mode == 2 ? ipo_js_bool(0) : ipo_js_num(-1);
}

static ipo_js_val_t js_arr_hof(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	/* mode: 0=forEach, 1=map, 2=filter, 3=find, 4=findIndex, 5=some, 6=every, 7=flatMap */
	int mode = (int)(intptr_t)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY || argc < 1 || !ipo_js_is_callable(argv[0]))
		return ipo_js_undef();
	ipo_js_obj_t *a = tv.u.obj;
	ipo_js_val_t cb = argv[0];
	ipo_js_val_t this_arg = argc >= 2 ? argv[1] : ipo_js_undef();
	ipo_js_obj_t *out = (mode == 1 || mode == 2 || mode == 7) ? ipo_js_new_array(vm, 0) : NULL;
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(out));
	for (size_t i = 0; i < a->u.arr.len; i++) {
		ipo_js_val_t args[3] = { a->u.arr.items[i], ipo_js_num((double)i), tv };
		ipo_js_val_t r = vm_call_obj(vm, cb, this_arg, 3, args, NULL);
		if (vm->sig != SIG_NONE) { ipo_js_gc_pop_roots(vm, top); return ipo_js_undef(); }
		if (mode == 1) ipo_js_arr_push(vm, out, r);
		else if (mode == 2 && ipo_js_to_bool(r)) ipo_js_arr_push(vm, out, a->u.arr.items[i]);
		else if (mode == 3 && ipo_js_to_bool(r)) { ipo_js_gc_pop_roots(vm, top); return a->u.arr.items[i]; }
		else if (mode == 4 && ipo_js_to_bool(r)) { ipo_js_gc_pop_roots(vm, top); return ipo_js_num((double)i); }
		else if (mode == 5 && ipo_js_to_bool(r)) { ipo_js_gc_pop_roots(vm, top); return ipo_js_bool(1); }
		else if (mode == 6 && !ipo_js_to_bool(r)) { ipo_js_gc_pop_roots(vm, top); return ipo_js_bool(0); }
		else if (mode == 7) {
			if (r.type == JS_T_OBJECT && r.u.obj && r.u.obj->kind == JS_OBJ_ARRAY) {
				for (size_t j = 0; j < r.u.obj->u.arr.len; j++) ipo_js_arr_push(vm, out, r.u.obj->u.arr.items[j]);
			} else ipo_js_arr_push(vm, out, r);
		}
	}
	ipo_js_gc_pop_roots(vm, top);
	if (mode == 4) return ipo_js_num(-1);
	if (mode == 5) return ipo_js_bool(0);
	if (mode == 6) return ipo_js_bool(1);
	return out ? ipo_js_obj_val(out) : ipo_js_undef();
}

static ipo_js_val_t js_arr_reduce(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int right = (int)(intptr_t)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY || argc < 1 || !ipo_js_is_callable(argv[0]))
		return ipo_js_undef();
	ipo_js_obj_t *a = tv.u.obj;
	if (a->u.arr.len == 0 && argc < 2) {
		ipo_js_throw_msg(vm, "TypeError", "Reduce of empty array with no initial value");
		return ipo_js_undef();
	}
	ipo_js_val_t acc;
	size_t idx;
	if (argc >= 2) { acc = argv[1]; idx = right ? a->u.arr.len : 0; }
	else if (!right) { acc = a->u.arr.items[0]; idx = 1; }
	else { acc = a->u.arr.items[a->u.arr.len - 1]; idx = a->u.arr.len - 1; }

	size_t top = ipo_js_gc_push_root(vm, acc);
	if (!right) {
		for (; idx < a->u.arr.len; idx++) {
			ipo_js_val_t args[4] = { acc, a->u.arr.items[idx], ipo_js_num((double)idx), tv };
			acc = vm_call_obj(vm, argv[0], ipo_js_undef(), 4, args, NULL);
			vm->root_stack[top] = acc;
			if (vm->sig != SIG_NONE) break;
		}
	} else {
		while (idx > 0) {
			idx--;
			ipo_js_val_t args[4] = { acc, a->u.arr.items[idx], ipo_js_num((double)idx), tv };
			acc = vm_call_obj(vm, argv[0], ipo_js_undef(), 4, args, NULL);
			vm->root_stack[top] = acc;
			if (vm->sig != SIG_NONE) break;
		}
	}
	ipo_js_gc_pop_roots(vm, top);
	return acc;
}

static void arr_flat_rec(ipo_js_vm_t *vm, ipo_js_obj_t *out, ipo_js_obj_t *src, int depth) {
	for (size_t i = 0; i < src->u.arr.len; i++) {
		ipo_js_val_t v = src->u.arr.items[i];
		if (depth > 0 && v.type == JS_T_OBJECT && v.u.obj && v.u.obj->kind == JS_OBJ_ARRAY) {
			arr_flat_rec(vm, out, v.u.obj, depth - 1);
		} else {
			ipo_js_arr_push(vm, out, v);
		}
	}
}

static ipo_js_val_t js_arr_flat(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_ARRAY) return ipo_js_obj_val(ipo_js_new_array(vm, 0));
	int d = (argc >= 1) ? ipo_js_to_i32(vm, argv[0]) : 1;
	ipo_js_obj_t *out = ipo_js_new_array(vm, 0);
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(out));
	arr_flat_rec(vm, out, tv.u.obj, d);
	ipo_js_gc_pop_roots(vm, top);
	return ipo_js_obj_val(out);
}

/* String statics & methods */
static ipo_js_val_t js_str_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_str_cstr(vm, "");
	return (ipo_js_val_t){.type = JS_T_STRING, .u.str = ipo_js_to_str(vm, argv[0])};
}

static ipo_js_val_t js_str_from_char_code(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	js_buf_t b; jbuf_init(&b);
	for (int i = 0; i < argc; i++) jbuf_append_utf8(&b, (uint32_t)ipo_js_to_i32(vm, argv[i]));
	ipo_js_val_t r = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
	jbuf_free(&b);
	return r;
}

static ipo_js_val_t js_str_char_at(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int code = (int)(intptr_t)ud;
	size_t len = 0;
	const char *s = ipo_js_cstr(vm, tv, &len);
	int32_t idx = (argc >= 1) ? ipo_js_to_i32(vm, argv[0]) : 0;
	if (idx < 0 || (size_t)idx >= len) return code ? ipo_js_num(NAN) : ipo_js_str_cstr(vm, "");
	return code ? ipo_js_num((double)(unsigned char)s[idx]) : ipo_js_str_new(vm, s + idx, 1);
}

static ipo_js_val_t js_str_search_basic(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	/* mode: 0=indexOf, 1=lastIndexOf, 2=includes, 3=startsWith, 4=endsWith */
	int mode = (int)(intptr_t)ud;
	size_t hlen = 0, nlen = 0;
	const char *hay = ipo_js_cstr(vm, tv, &hlen);
	size_t top = ipo_js_gc_push_root(vm, ipo_js_str_new(vm, hay, hlen));
	const char *ned = (argc >= 1) ? ipo_js_cstr(vm, argv[0], &nlen) : "undefined";
	if (argc < 1) nlen = 9;
	if (mode == 3) {
		size_t pos = (argc >= 2) ? norm_idx(ipo_js_to_i32(vm, argv[1]), hlen) : 0;
		int ok = (pos + nlen <= hlen && memcmp(hay + pos, ned, nlen) == 0);
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_bool(ok);
	}
	if (mode == 4) {
		size_t endp = (argc >= 2 && argv[1].type != JS_T_UNDEFINED) ? norm_idx(ipo_js_to_i32(vm, argv[1]), hlen) : hlen;
		int ok = (nlen <= endp && memcmp(hay + endp - nlen, ned, nlen) == 0);
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_bool(ok);
	}
	if (mode == 1) {
		size_t from = (argc >= 2 && argv[1].type != JS_T_UNDEFINED) ? norm_idx(ipo_js_to_i32(vm, argv[1]), hlen) : hlen;
		if (nlen == 0) { ipo_js_gc_pop_roots(vm, top); return ipo_js_num((double)from); }
		for (size_t i = from < hlen - nlen ? from : hlen - nlen; ; i--) {
			if (i + nlen <= hlen && memcmp(hay + i, ned, nlen) == 0) {
				ipo_js_gc_pop_roots(vm, top); return ipo_js_num((double)i);
			}
			if (i == 0) break;
		}
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_num(-1);
	}
	size_t from = (argc >= 2) ? norm_idx(ipo_js_to_i32(vm, argv[1]), hlen) : 0;
	int found = -1;
	if (nlen == 0 && from <= hlen) found = (int)from;
	else if (from < hlen && nlen <= hlen - from) {
		for (size_t i = from; i <= hlen - nlen; i++) {
			if (memcmp(hay + i, ned, nlen) == 0) { found = (int)i; break; }
		}
	}
	ipo_js_gc_pop_roots(vm, top);
	return (mode == 2) ? ipo_js_bool(found >= 0) : ipo_js_num((double)found);
}

static ipo_js_val_t js_str_slice(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int kind = (int)(intptr_t)ud; /* 0=slice, 1=substring, 2=substr */
	size_t len = 0;
	const char *s = ipo_js_cstr(vm, tv, &len);
	size_t a = 0, b = len;
	if (kind == 0) {
		a = (argc >= 1 && argv[0].type != JS_T_UNDEFINED) ? norm_idx(ipo_js_to_i32(vm, argv[0]), len) : 0;
		b = (argc >= 2 && argv[1].type != JS_T_UNDEFINED) ? norm_idx(ipo_js_to_i32(vm, argv[1]), len) : len;
		if (b < a) b = a;
	} else if (kind == 1) {
		int32_t x = (argc >= 1) ? ipo_js_to_i32(vm, argv[0]) : 0;
		int32_t y = (argc >= 2 && argv[1].type != JS_T_UNDEFINED) ? ipo_js_to_i32(vm, argv[1]) : (int32_t)len;
		if (x < 0) x = 0;
		if (y < 0) y = 0;
		a = (size_t)x > len ? len : (size_t)x;
		b = (size_t)y > len ? len : (size_t)y;
		if (b < a) { size_t t = a; a = b; b = t; }
	} else {
		a = (argc >= 1) ? norm_idx(ipo_js_to_i32(vm, argv[0]), len) : 0;
		int32_t l = (argc >= 2 && argv[1].type != JS_T_UNDEFINED) ? ipo_js_to_i32(vm, argv[1]) : (int32_t)(len - a);
		b = (l > 0) ? (a + (size_t)l) : a;
		if (b > len) b = len;
	}
	return ipo_js_str_new(vm, s + a, b - a);
}

static ipo_js_val_t js_str_case_trim(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)argc; (void)argv;
	int mode = (int)(intptr_t)ud; /* 0=lower, 1=upper, 2=trim, 3=trimStart, 4=trimEnd */
	size_t len = 0;
	const char *s = ipo_js_cstr(vm, tv, &len);
	if (mode <= 1) {
		char *tmp = xstrdup_n(s, len);
		for (size_t i = 0; i < len; i++) {
			tmp[i] = (char)(mode == 0 ? tolower((unsigned char)tmp[i]) : toupper((unsigned char)tmp[i]));
		}
		ipo_js_val_t r = ipo_js_str_new(vm, tmp, len);
		free(tmp);
		return r;
	}
	size_t a = 0, b = len;
	if (mode == 2 || mode == 3) while (a < b && isspace((unsigned char)s[a])) a++;
	if (mode == 2 || mode == 4) while (b > a && isspace((unsigned char)s[b - 1])) b--;
	return ipo_js_str_new(vm, s + a, b - a);
}

static ipo_js_val_t js_str_split(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	size_t len = 0;
	const char *s = ipo_js_cstr(vm, tv, &len);
	ipo_js_obj_t *out = ipo_js_new_array(vm, 0);
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(out));
	size_t limit = (argc >= 2 && argv[1].type != JS_T_UNDEFINED) ? (size_t)ipo_js_to_i32(vm, argv[1]) : 1000000;
	if (argc < 1 || argv[0].type == JS_T_UNDEFINED) {
		if (limit > 0) ipo_js_arr_push(vm, out, ipo_js_str_new(vm, s, len));
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_obj_val(out);
	}
	if (argv[0].type == JS_T_OBJECT && argv[0].u.obj && argv[0].u.obj->kind == JS_OBJ_REGEXP) {
		ipo_js_obj_t *re = argv[0].u.obj;
		re_prog_t *prog = re_compile(re->u.re.pattern->data, re->u.re.pattern->len, re->u.re.flags);
		size_t pos = 0, ms = 0, me = 0;
		while (out->u.arr.len < limit && pos <= len) {
			if (!re_exec_at(prog, s, len, pos, NULL, &ms, &me)) {
				ipo_js_arr_push(vm, out, ipo_js_str_new(vm, s + pos, len - pos));
				break;
			}
			if (me == ms && ms == pos && pos < len) {
				ipo_js_arr_push(vm, out, ipo_js_str_new(vm, s + pos, 1));
				pos++;
				continue;
			}
			ipo_js_arr_push(vm, out, ipo_js_str_new(vm, s + pos, ms - pos));
			pos = me;
			if (pos == len) {
				if (me > ms && out->u.arr.len < limit) ipo_js_arr_push(vm, out, ipo_js_str_cstr(vm, ""));
				break;
			}
		}
		re_free(prog);
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_obj_val(out);
	}
	size_t slen = 0;
	const char *sep = ipo_js_cstr(vm, argv[0], &slen);
	if (slen == 0) {
		for (size_t i = 0; i < len && out->u.arr.len < limit; i++) {
			ipo_js_arr_push(vm, out, ipo_js_str_new(vm, s + i, 1));
		}
		ipo_js_gc_pop_roots(vm, top);
		return ipo_js_obj_val(out);
	}
	size_t pos = 0;
	while (out->u.arr.len < limit) {
		const char *f = NULL;
		if (pos + slen <= len) {
			for (size_t i = pos; i <= len - slen; i++) {
				if (memcmp(s + i, sep, slen) == 0) { f = s + i; break; }
			}
		}
		if (!f) {
			ipo_js_arr_push(vm, out, ipo_js_str_new(vm, s + pos, len - pos));
			break;
		}
		ipo_js_arr_push(vm, out, ipo_js_str_new(vm, s + pos, (size_t)(f - (s + pos))));
		pos = (size_t)(f - s) + slen;
	}
	ipo_js_gc_pop_roots(vm, top);
	return ipo_js_obj_val(out);
}

static ipo_js_val_t js_str_replace(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int rep_all = (int)(intptr_t)ud;
	size_t slen = 0;
	const char *s = ipo_js_cstr(vm, tv, &slen);
	if (argc < 2) return ipo_js_str_new(vm, s, slen);
	int is_fn = ipo_js_is_callable(argv[1]);
	size_t rlen = 0;
	const char *rep = is_fn ? "" : ipo_js_cstr(vm, argv[1], &rlen);

	js_buf_t b; jbuf_init(&b);
	if (argv[0].type == JS_T_OBJECT && argv[0].u.obj && argv[0].u.obj->kind == JS_OBJ_REGEXP) {
		ipo_js_obj_t *re = argv[0].u.obj;
		int global = rep_all || ((re->u.re.flags & 2) != 0);
		re_prog_t *prog = re_compile(re->u.re.pattern->data, re->u.re.pattern->len, re->u.re.flags);
		re_state_t st;
		size_t pos = 0, ms = 0, me = 0;
		while (pos <= slen && re_exec_at(prog, s, slen, pos, &st, &ms, &me)) {
			jbuf_append(&b, s + pos, ms - pos);
			if (is_fn) {
				ipo_js_val_t args[4];
				int ac = 0;
				args[ac++] = ipo_js_str_new(vm, s + ms, me - ms);
				for (int g = 1; g <= prog->num_groups && ac < 3; g++) {
					if (st.cap_start[g] >= 0 && st.cap_end[g] >= st.cap_start[g])
						args[ac++] = ipo_js_str_new(vm, s + st.cap_start[g], (size_t)(st.cap_end[g] - st.cap_start[g]));
					else args[ac++] = ipo_js_undef();
				}
				args[ac++] = ipo_js_num((double)ms);
				ipo_js_val_t rv = vm_call_obj(vm, argv[1], ipo_js_undef(), ac, args, NULL);
				size_t flen = 0;
				const char *fs = ipo_js_cstr(vm, rv, &flen);
				jbuf_append(&b, fs, flen);
			} else {
				jbuf_append(&b, rep, rlen);
			}
			pos = (me > ms) ? me : (me + 1);
			if (me == ms && ms < slen) jbuf_append_ch(&b, s[ms]);
			if (!global) break;
		}
		if (pos <= slen) jbuf_append(&b, s + pos, slen - pos);
		re_free(prog);
	} else {
		size_t nlen = 0;
		const char *ned = ipo_js_cstr(vm, argv[0], &nlen);
		size_t pos = 0;
		while (pos <= slen) {
			const char *f = NULL;
			if (nlen == 0) f = s + pos;
			else if (pos + nlen <= slen) {
				for (size_t i = pos; i <= slen - nlen; i++) {
					if (memcmp(s + i, ned, nlen) == 0) { f = s + i; break; }
				}
			}
			if (!f) break;
			jbuf_append(&b, s + pos, (size_t)(f - (s + pos)));
			if (is_fn) {
				ipo_js_val_t args[3] = { ipo_js_str_new(vm, ned, nlen), ipo_js_num((double)(f - s)), tv };
				ipo_js_val_t rv = vm_call_obj(vm, argv[1], ipo_js_undef(), 3, args, NULL);
				size_t flen = 0; const char *fs = ipo_js_cstr(vm, rv, &flen);
				jbuf_append(&b, fs, flen);
			} else {
				jbuf_append(&b, rep, rlen);
			}
			pos = (size_t)(f - s) + (nlen ? nlen : 1);
			if (nlen == 0 && f - s < (ptrdiff_t)slen) jbuf_append_ch(&b, *f);
			if (!rep_all) break;
		}
		if (pos <= slen) jbuf_append(&b, s + pos, slen - pos);
	}
	ipo_js_val_t out = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
	jbuf_free(&b);
	return out;
}

static ipo_js_val_t js_str_match(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	size_t slen = 0;
	const char *s = ipo_js_cstr(vm, tv, &slen);
	if (argc < 1) return ipo_js_null();
	ipo_js_obj_t *re = NULL;
	if (argv[0].type == JS_T_OBJECT && argv[0].u.obj && argv[0].u.obj->kind == JS_OBJ_REGEXP) {
		re = argv[0].u.obj;
	} else {
		size_t plen = 0; const char *p = ipo_js_cstr(vm, argv[0], &plen);
		re = vm_new_regexp(vm, p, plen, 0);
	}
	re_prog_t *prog = re_compile(re->u.re.pattern->data, re->u.re.pattern->len, re->u.re.flags);
	re_state_t st;
	size_t ms = 0, me = 0;
	if (re->u.re.flags & 2) {
		ipo_js_obj_t *arr = ipo_js_new_array(vm, 0);
		size_t pos = 0;
		while (pos <= slen && re_exec_at(prog, s, slen, pos, &st, &ms, &me)) {
			ipo_js_arr_push(vm, arr, ipo_js_str_new(vm, s + ms, me - ms));
			pos = (me > ms) ? me : (me + 1);
		}
		re_free(prog);
		return arr->u.arr.len ? ipo_js_obj_val(arr) : ipo_js_null();
	}
	if (!re_exec_at(prog, s, slen, 0, &st, &ms, &me)) { re_free(prog); return ipo_js_null(); }
	ipo_js_obj_t *arr = ipo_js_new_array(vm, 0);
	ipo_js_arr_push(vm, arr, ipo_js_str_new(vm, s + ms, me - ms));
	for (int g = 1; g <= prog->num_groups; g++) {
		if (st.cap_start[g] >= 0 && st.cap_end[g] >= st.cap_start[g])
			ipo_js_arr_push(vm, arr, ipo_js_str_new(vm, s + st.cap_start[g], (size_t)(st.cap_end[g] - st.cap_start[g])));
		else ipo_js_arr_push(vm, arr, ipo_js_undef());
	}
	ipo_js_set_prop(vm, arr, "index", ipo_js_num((double)ms));
	re_free(prog);
	return ipo_js_obj_val(arr);
}

static ipo_js_val_t js_str_repeat_pad(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int mode = (int)(intptr_t)ud; /* 0=repeat, 1=padStart, 2=padEnd */
	size_t len = 0;
	const char *s = ipo_js_cstr(vm, tv, &len);
	int32_t cnt = (argc >= 1) ? ipo_js_to_i32(vm, argv[0]) : 0;
	if (cnt < 0) cnt = 0;
	if (cnt > 100000) cnt = 100000;
	js_buf_t b; jbuf_init(&b);
	if (mode == 0) {
		for (int32_t i = 0; i < cnt; i++) jbuf_append(&b, s, len);
	} else {
		if ((size_t)cnt <= len) return ipo_js_str_new(vm, s, len);
		size_t plen = 1;
		const char *pad = (argc >= 2 && argv[1].type != JS_T_UNDEFINED) ? ipo_js_cstr(vm, argv[1], &plen) : " ";
		if (plen == 0) { jbuf_free(&b); return ipo_js_str_new(vm, s, len); }
		size_t need = (size_t)cnt - len;
		if (mode == 2) jbuf_append(&b, s, len);
		for (size_t i = 0; i < need; i++) jbuf_append_ch(&b, pad[i % plen]);
		if (mode == 1) jbuf_append(&b, s, len);
	}
	ipo_js_val_t r = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
	jbuf_free(&b);
	return r;
}

/* RegExp ctor & methods */
static ipo_js_val_t js_re_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	const char *pat = (argc >= 1 && argv[0].type != JS_T_UNDEFINED) ? ipo_js_cstr(vm, argv[0], NULL) : "";
	const char *fl  = (argc >= 2 && argv[1].type != JS_T_UNDEFINED) ? ipo_js_cstr(vm, argv[1], NULL) : "";
	return ipo_js_obj_val(vm_new_regexp(vm, pat, strlen(pat), parse_re_flags(fl)));
}

static ipo_js_val_t js_re_test_exec(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int is_exec = (int)(intptr_t)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_REGEXP)
		return is_exec ? ipo_js_null() : ipo_js_bool(0);
	ipo_js_obj_t *re = tv.u.obj;
	size_t slen = 0;
	const char *s = (argc >= 1) ? ipo_js_cstr(vm, argv[0], &slen) : "";
	size_t start = (re->u.re.flags & 2) ? re->u.re.last_index : 0;
	if (start > slen) {
		if (re->u.re.flags & 2) re->u.re.last_index = 0;
		return is_exec ? ipo_js_null() : ipo_js_bool(0);
	}
	re_prog_t *prog = re_compile(re->u.re.pattern->data, re->u.re.pattern->len, re->u.re.flags);
	re_state_t st;
	size_t ms = 0, me = 0;
	int ok = re_exec_at(prog, s, slen, start, &st, &ms, &me);
	if (!ok) {
		if (re->u.re.flags & 2) re->u.re.last_index = 0;
		re_free(prog);
		return is_exec ? ipo_js_null() : ipo_js_bool(0);
	}
	if (re->u.re.flags & 2) re->u.re.last_index = me;
	if (!is_exec) { re_free(prog); return ipo_js_bool(1); }
	ipo_js_obj_t *arr = ipo_js_new_array(vm, 0);
	ipo_js_arr_push(vm, arr, ipo_js_str_new(vm, s + ms, me - ms));
	for (int g = 1; g <= prog->num_groups; g++) {
		if (st.cap_start[g] >= 0 && st.cap_end[g] >= st.cap_start[g])
			ipo_js_arr_push(vm, arr, ipo_js_str_new(vm, s + st.cap_start[g], (size_t)(st.cap_end[g] - st.cap_start[g])));
		else ipo_js_arr_push(vm, arr, ipo_js_undef());
	}
	ipo_js_set_prop(vm, arr, "index", ipo_js_num((double)ms));
	re_free(prog);
	return ipo_js_obj_val(arr);
}

/* Number / Boolean / Math / Date / JSON / Error / Promise */
static ipo_js_val_t js_num_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	return argc < 1 ? ipo_js_num(0) : ipo_js_num(ipo_js_to_num(vm, argv[0]));
}

static ipo_js_val_t js_bool_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)vm; (void)tv; (void)ud;
	return argc < 1 ? ipo_js_bool(0) : ipo_js_bool(ipo_js_to_bool(argv[0]));
}

static ipo_js_val_t js_math_1(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int op = (int)(intptr_t)ud;
	double x = (argc >= 1) ? ipo_js_to_num(vm, argv[0]) : NAN;
	switch (op) {
	case 0: return ipo_js_num(fabs(x));
	case 1: return ipo_js_num(floor(x));
	case 2: return ipo_js_num(ceil(x));
	case 3: return ipo_js_num(floor(x + 0.5));
	case 4: return ipo_js_num(trunc(x));
	case 5: return ipo_js_num(sqrt(x));
	case 6: return ipo_js_num(sin(x));
	case 7: return ipo_js_num(cos(x));
	case 8: return ipo_js_num(tan(x));
	case 9: return ipo_js_num(log(x));
	case 10: return ipo_js_num(exp(x));
	case 11: return ipo_js_num(isnan(x) ? NAN : (x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0)));
	case 12: return ipo_js_num(log10(x));
	case 13: return ipo_js_num(log2(x));
	case 14: return ipo_js_num(atan(x));
	}
	return ipo_js_num(NAN);
}

static ipo_js_val_t js_math_pow(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	double a = argc >= 1 ? ipo_js_to_num(vm, argv[0]) : NAN;
	double b = argc >= 2 ? ipo_js_to_num(vm, argv[1]) : NAN;
	return ipo_js_num(pow(a, b));
}

static ipo_js_val_t js_math_atan2(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	double a = argc >= 1 ? ipo_js_to_num(vm, argv[0]) : NAN;
	double b = argc >= 2 ? ipo_js_to_num(vm, argv[1]) : NAN;
	return ipo_js_num(atan2(a, b));
}

static ipo_js_val_t js_math_minmax(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int is_max = (int)(intptr_t)ud;
	double best = is_max ? -INFINITY : INFINITY;
	for (int i = 0; i < argc; i++) {
		double d = ipo_js_to_num(vm, argv[i]);
		if (isnan(d)) return ipo_js_num(NAN);
		if (is_max ? (d > best) : (d < best)) best = d;
	}
	return ipo_js_num(best);
}

static ipo_js_val_t js_math_random(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)argc; (void)argv; (void)ud;
	uint32_t x = vm->rng_state ? vm->rng_state : 0x12345678u;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	vm->rng_state = x;
	return ipo_js_num((double)(x & 0xFFFFFFFFu) / 4294967296.0);
}

static double now_epoch_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
}

static ipo_js_val_t js_date_now(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)vm; (void)tv; (void)argc; (void)argv; (void)ud;
	return ipo_js_num(now_epoch_ms());
}

static ipo_js_val_t js_date_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	ipo_js_obj_t *d = (tv.type == JS_T_OBJECT && tv.u.obj) ? tv.u.obj : vm_alloc_obj(vm, JS_OBJ_DATE, vm->date_proto);
	d->kind = JS_OBJ_DATE;
	d->proto = vm->date_proto;
	if (argc >= 1 && argv[0].type == JS_T_NUMBER) d->u.date.ms = argv[0].u.num;
	else d->u.date.ms = now_epoch_ms();
	return ipo_js_obj_val(d);
}

static ipo_js_val_t js_date_get(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)argc; (void)argv;
	int what = (int)(intptr_t)ud;
	double ms = (tv.type == JS_T_OBJECT && tv.u.obj && tv.u.obj->kind == JS_OBJ_DATE) ? tv.u.obj->u.date.ms : now_epoch_ms();
	if (what == 0) return ipo_js_num(ms);
	time_t t = (time_t)(ms / 1000.0);
	struct tm tm_v;
	struct tm *tm = gmtime_r(&t, &tm_v);
	if (!tm) return ipo_js_num(0);
	if (what == 1) return ipo_js_num((double)(tm->tm_year + 1900));
	if (what == 2) return ipo_js_num((double)tm->tm_mon);
	if (what == 3) return ipo_js_num((double)tm->tm_mday);
	if (what == 4) return ipo_js_num((double)tm->tm_wday);
	if (what == 5) return ipo_js_num((double)tm->tm_hour);
	if (what == 6) return ipo_js_num((double)tm->tm_min);
	if (what == 7) return ipo_js_num((double)tm->tm_sec);
	if (what == 8) return ipo_js_num((double)((long long)ms % 1000));
	char buf[64];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
	         tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
	         tm->tm_hour, tm->tm_min, tm->tm_sec, (int)((long long)ms % 1000));
	return ipo_js_str_cstr(vm, buf);
}

static ipo_js_val_t js_json_parse(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) { ipo_js_throw_msg(vm, "SyntaxError", "Unexpected end of JSON"); return ipo_js_undef(); }
	size_t len = 0;
	const char *s = ipo_js_cstr(vm, argv[0], &len);
	ipo_js_val_t out = ipo_js_undef();
	ipo_js_json_parse(vm, s, len, &out);
	return out;
}

static ipo_js_val_t js_json_stringify_nat(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_undef();
	int indent = (argc >= 3 && argv[2].type == JS_T_NUMBER) ? ipo_js_to_i32(vm, argv[2]) : 0;
	if (indent < 0) indent = 0;
	if (indent > 8) indent = 8;
	ipo_js_str_t *s = ipo_js_json_stringify(vm, argv[0], indent);
	if (!s) return ipo_js_undef();
	return (ipo_js_val_t){.type = JS_T_STRING, .u.str = s};
}

static ipo_js_val_t js_err_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	const char *name = (const char *)ud;
	const char *msg = (argc >= 1 && argv[0].type != JS_T_UNDEFINED) ? ipo_js_cstr(vm, argv[0], NULL) : "";
	return ipo_js_obj_val(ipo_js_new_error(vm, name, msg));
}

/* Promise natives */
static ipo_js_val_t js_prom_resolve_cb(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	ipo_js_obj_t *p = (ipo_js_obj_t *)ud;
	promise_settle(vm, p, 1, argc >= 1 ? argv[0] : ipo_js_undef());
	return ipo_js_undef();
}

static ipo_js_val_t js_prom_reject_cb(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	ipo_js_obj_t *p = (ipo_js_obj_t *)ud;
	promise_settle(vm, p, 2, argc >= 1 ? argv[0] : ipo_js_undef());
	return ipo_js_undef();
}

static ipo_js_val_t js_prom_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	ipo_js_obj_t *p = ipo_js_new_promise(vm);
	if (argc >= 1 && ipo_js_is_callable(argv[0])) {
		ipo_js_val_t res_fn = ipo_js_obj_val(ipo_js_new_native_fn(vm, "resolve", js_prom_resolve_cb, p, 0));
		ipo_js_val_t rej_fn = ipo_js_obj_val(ipo_js_new_native_fn(vm, "reject",  js_prom_reject_cb,  p, 0));
		ipo_js_val_t args[2] = { res_fn, rej_fn };
		vm_call_obj(vm, argv[0], ipo_js_undef(), 2, args, NULL);
		if (vm->sig == SIG_THROW) {
			ipo_js_val_t ev = vm->exc_val;
			ipo_js_clear_exception(vm);
			promise_settle(vm, p, 2, ev);
		}
	}
	return ipo_js_obj_val(p);
}

static ipo_js_val_t js_prom_static_settle(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int st = (int)(intptr_t)ud;
	ipo_js_val_t v = argc >= 1 ? argv[0] : ipo_js_undef();
	if (st == 1 && v.type == JS_T_OBJECT && v.u.obj && v.u.obj->kind == JS_OBJ_PROMISE) return v;
	ipo_js_obj_t *p = ipo_js_new_promise(vm);
	promise_settle(vm, p, st, v);
	return ipo_js_obj_val(p);
}

static ipo_js_val_t js_prom_then(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int is_catch = (int)(intptr_t)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || tv.u.obj->kind != JS_OBJ_PROMISE) return ipo_js_undef();
	ipo_js_obj_t *p = tv.u.obj;
	ipo_js_obj_t *next = ipo_js_new_promise(vm);
	ipo_js_val_t on_ful = (!is_catch && argc >= 1) ? argv[0] : ipo_js_undef();
	ipo_js_val_t on_rej = is_catch ? (argc >= 1 ? argv[0] : ipo_js_undef())
	                               : (argc >= 2 ? argv[1] : ipo_js_undef());
	if (p->u.prom.state == 1) {
		vm_enqueue_microtask(vm, on_ful, p->u.prom.result, next, 0);
	} else if (p->u.prom.state == 2) {
		vm_enqueue_microtask(vm, on_rej, p->u.prom.result, next, 1);
	} else {
		if (p->u.prom.cb_len >= p->u.prom.cb_cap) {
			int ncap = p->u.prom.cb_cap ? p->u.prom.cb_cap * 2 : 4;
			p->u.prom.then_fns = (ipo_js_val_t *)xrealloc(p->u.prom.then_fns, (size_t)ncap * sizeof(ipo_js_val_t));
			p->u.prom.catch_fns = (ipo_js_val_t *)xrealloc(p->u.prom.catch_fns, (size_t)ncap * sizeof(ipo_js_val_t));
			p->u.prom.then_promises = (ipo_js_val_t *)xrealloc(p->u.prom.then_promises, (size_t)ncap * sizeof(ipo_js_val_t));
			p->u.prom.cb_cap = ncap;
		}
		int idx = p->u.prom.cb_len++;
		p->u.prom.then_fns[idx] = on_ful;
		p->u.prom.catch_fns[idx] = on_rej;
		p->u.prom.then_promises[idx] = ipo_js_obj_val(next);
	}
	return ipo_js_obj_val(next);
}

/* =========================================================================
 * VM Initialization & Standard Library Registration
 * ========================================================================= */

static void add_fn(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *name,
                   ipo_js_native_fn_t fn, void *ud, int is_ctor) {
	ipo_js_obj_t *f = ipo_js_new_native_fn(vm, name, fn, ud, is_ctor);
	ipo_js_set_prop(vm, obj, name, ipo_js_obj_val(f));
}

ipo_js_vm_t *ipo_js_vm_new(void *host_ctx, int max_call_depth, int max_steps) {
	ipo_js_vm_t *vm = (ipo_js_vm_t *)xcalloc(1, sizeof(ipo_js_vm_t));
	vm->host_ctx = host_ctx;
	vm->max_depth = max_call_depth > 0 ? max_call_depth : 100;
	vm->max_steps = max_steps > 0 ? max_steps : 2000000;
	vm->gc_threshold = 512 * 1024;
	vm->rng_state = (uint32_t)time(NULL) ^ 0x9E3779B9u;

	vm->obj_proto     = vm_alloc_obj(vm, JS_OBJ_PLAIN, NULL);
	vm->fn_proto      = vm_alloc_obj(vm, JS_OBJ_PLAIN, vm->obj_proto);
	vm->arr_proto     = vm_alloc_obj(vm, JS_OBJ_PLAIN, vm->obj_proto);
	vm->str_proto     = vm_alloc_obj(vm, JS_OBJ_PLAIN, vm->obj_proto);
	vm->err_proto     = vm_alloc_obj(vm, JS_OBJ_PLAIN, vm->obj_proto);
	vm->regexp_proto  = vm_alloc_obj(vm, JS_OBJ_PLAIN, vm->obj_proto);
	vm->date_proto    = vm_alloc_obj(vm, JS_OBJ_PLAIN, vm->obj_proto);
	vm->promise_proto = vm_alloc_obj(vm, JS_OBJ_PLAIN, vm->obj_proto);

	vm->global_obj = ipo_js_new_object(vm, vm->obj_proto);
	vm->global_env = vm_alloc_env(vm, NULL);
	vm->global_env->is_func_scope = 1;
	vm->global_env->with_obj = vm->global_obj;
	vm->global_env->has_this = 1;
	vm->global_env->this_val = ipo_js_obj_val(vm->global_obj);

	ipo_js_obj_t *g = vm->global_obj;
	ipo_js_set_prop(vm, g, "globalThis", ipo_js_obj_val(g));
	ipo_js_set_prop(vm, g, "undefined",  ipo_js_undef());
	ipo_js_set_prop(vm, g, "NaN",        ipo_js_num(NAN));
	ipo_js_set_prop(vm, g, "Infinity",   ipo_js_num(INFINITY));

	add_fn(vm, g, "parseInt",           js_nat_parse_int,   NULL, 0);
	add_fn(vm, g, "parseFloat",         js_nat_parse_float, NULL, 0);
	add_fn(vm, g, "isNaN",              js_nat_is_nan,      NULL, 0);
	add_fn(vm, g, "isFinite",           js_nat_is_finite,   NULL, 0);
	add_fn(vm, g, "encodeURI",          js_nat_encode_uri,  (void *)1, 0);
	add_fn(vm, g, "encodeURIComponent", js_nat_encode_uri,  (void *)0, 0);
	add_fn(vm, g, "decodeURI",          js_nat_decode_uri,  (void *)1, 0);
	add_fn(vm, g, "decodeURIComponent", js_nat_decode_uri,  (void *)0, 0);
	add_fn(vm, g, "eval",               js_nat_eval,        NULL, 0);

	/* Function.prototype */
	add_fn(vm, vm->fn_proto, "call",  js_fn_call_apply, (void *)0, 0);
	add_fn(vm, vm->fn_proto, "apply", js_fn_call_apply, (void *)1, 0);
	add_fn(vm, vm->fn_proto, "bind",  js_fn_bind,       NULL, 0);

	/* Object */
	add_fn(vm, vm->obj_proto, "hasOwnProperty", js_obj_has_own, NULL, 0);
	add_fn(vm, vm->obj_proto, "toString",       js_obj_to_str,  NULL, 0);
	ipo_js_obj_t *obj_ctor = ipo_js_new_native_fn(vm, "Object", js_obj_ctor, NULL, 1);
	ipo_js_set_prop(vm, obj_ctor, "prototype", ipo_js_obj_val(vm->obj_proto));
	add_fn(vm, obj_ctor, "keys",           js_obj_keys_vals_entries, (void *)0, 0);
	add_fn(vm, obj_ctor, "values",         js_obj_keys_vals_entries, (void *)1, 0);
	add_fn(vm, obj_ctor, "entries",        js_obj_keys_vals_entries, (void *)2, 0);
	add_fn(vm, obj_ctor, "assign",         js_obj_assign,            NULL, 0);
	add_fn(vm, obj_ctor, "create",         js_obj_create,            NULL, 0);
	add_fn(vm, obj_ctor, "defineProperty", js_obj_define_prop,       NULL, 0);
	ipo_js_set_prop(vm, g, "Object", ipo_js_obj_val(obj_ctor));

	/* Array */
	add_fn(vm, vm->arr_proto, "push",        js_arr_push,     NULL, 0);
	add_fn(vm, vm->arr_proto, "pop",         js_arr_pop,      NULL, 0);
	add_fn(vm, vm->arr_proto, "shift",       js_arr_shift,    NULL, 0);
	add_fn(vm, vm->arr_proto, "unshift",     js_arr_unshift,  NULL, 0);
	add_fn(vm, vm->arr_proto, "join",        js_arr_join,     NULL, 0);
	add_fn(vm, vm->arr_proto, "slice",       js_arr_slice,    NULL, 0);
	add_fn(vm, vm->arr_proto, "splice",      js_arr_splice,   NULL, 0);
	add_fn(vm, vm->arr_proto, "concat",      js_arr_concat,   NULL, 0);
	add_fn(vm, vm->arr_proto, "reverse",     js_arr_reverse,  NULL, 0);
	add_fn(vm, vm->arr_proto, "sort",        js_arr_sort,     NULL, 0);
	add_fn(vm, vm->arr_proto, "indexOf",     js_arr_index_of, (void *)0, 0);
	add_fn(vm, vm->arr_proto, "lastIndexOf", js_arr_index_of, (void *)1, 0);
	add_fn(vm, vm->arr_proto, "includes",    js_arr_index_of, (void *)2, 0);
	add_fn(vm, vm->arr_proto, "forEach",     js_arr_hof,      (void *)0, 0);
	add_fn(vm, vm->arr_proto, "map",         js_arr_hof,      (void *)1, 0);
	add_fn(vm, vm->arr_proto, "filter",      js_arr_hof,      (void *)2, 0);
	add_fn(vm, vm->arr_proto, "find",        js_arr_hof,      (void *)3, 0);
	add_fn(vm, vm->arr_proto, "findIndex",   js_arr_hof,      (void *)4, 0);
	add_fn(vm, vm->arr_proto, "some",        js_arr_hof,      (void *)5, 0);
	add_fn(vm, vm->arr_proto, "every",       js_arr_hof,      (void *)6, 0);
	add_fn(vm, vm->arr_proto, "flatMap",     js_arr_hof,      (void *)7, 0);
	add_fn(vm, vm->arr_proto, "reduce",      js_arr_reduce,   (void *)0, 0);
	add_fn(vm, vm->arr_proto, "reduceRight", js_arr_reduce,   (void *)1, 0);
	add_fn(vm, vm->arr_proto, "flat",        js_arr_flat,     NULL, 0);
	add_fn(vm, vm->arr_proto, "toString",    js_obj_to_str,   NULL, 0);
	ipo_js_obj_t *arr_ctor = ipo_js_new_native_fn(vm, "Array", js_arr_ctor, NULL, 1);
	ipo_js_set_prop(vm, arr_ctor, "prototype", ipo_js_obj_val(vm->arr_proto));
	add_fn(vm, arr_ctor, "isArray", js_arr_is_array, NULL, 0);
	add_fn(vm, arr_ctor, "from",    js_arr_from,     NULL, 0);
	ipo_js_set_prop(vm, g, "Array", ipo_js_obj_val(arr_ctor));

	/* String */
	add_fn(vm, vm->str_proto, "charAt",      js_str_char_at,      (void *)0, 0);
	add_fn(vm, vm->str_proto, "charCodeAt",  js_str_char_at,      (void *)1, 0);
	add_fn(vm, vm->str_proto, "indexOf",     js_str_search_basic, (void *)0, 0);
	add_fn(vm, vm->str_proto, "lastIndexOf", js_str_search_basic, (void *)1, 0);
	add_fn(vm, vm->str_proto, "includes",    js_str_search_basic, (void *)2, 0);
	add_fn(vm, vm->str_proto, "startsWith",  js_str_search_basic, (void *)3, 0);
	add_fn(vm, vm->str_proto, "endsWith",    js_str_search_basic, (void *)4, 0);
	add_fn(vm, vm->str_proto, "slice",       js_str_slice,        (void *)0, 0);
	add_fn(vm, vm->str_proto, "substring",   js_str_slice,        (void *)1, 0);
	add_fn(vm, vm->str_proto, "substr",      js_str_slice,        (void *)2, 0);
	add_fn(vm, vm->str_proto, "toLowerCase", js_str_case_trim,    (void *)0, 0);
	add_fn(vm, vm->str_proto, "toUpperCase", js_str_case_trim,    (void *)1, 0);
	add_fn(vm, vm->str_proto, "trim",        js_str_case_trim,    (void *)2, 0);
	add_fn(vm, vm->str_proto, "trimStart",   js_str_case_trim,    (void *)3, 0);
	add_fn(vm, vm->str_proto, "trimEnd",     js_str_case_trim,    (void *)4, 0);
	add_fn(vm, vm->str_proto, "split",       js_str_split,        NULL, 0);
	add_fn(vm, vm->str_proto, "replace",     js_str_replace,      (void *)0, 0);
	add_fn(vm, vm->str_proto, "replaceAll",  js_str_replace,      (void *)1, 0);
	add_fn(vm, vm->str_proto, "match",       js_str_match,        NULL, 0);
	add_fn(vm, vm->str_proto, "repeat",      js_str_repeat_pad,   (void *)0, 0);
	add_fn(vm, vm->str_proto, "padStart",    js_str_repeat_pad,   (void *)1, 0);
	add_fn(vm, vm->str_proto, "padEnd",      js_str_repeat_pad,   (void *)2, 0);
	ipo_js_obj_t *str_ctor = ipo_js_new_native_fn(vm, "String", js_str_ctor, NULL, 1);
	ipo_js_set_prop(vm, str_ctor, "prototype", ipo_js_obj_val(vm->str_proto));
	add_fn(vm, str_ctor, "fromCharCode",  js_str_from_char_code, NULL, 0);
	add_fn(vm, str_ctor, "fromCodePoint", js_str_from_char_code, NULL, 0);
	ipo_js_set_prop(vm, g, "String", ipo_js_obj_val(str_ctor));

	/* Number & Boolean */
	ipo_js_obj_t *num_ctor = ipo_js_new_native_fn(vm, "Number", js_num_ctor, NULL, 1);
	add_fn(vm, num_ctor, "isNaN",      js_nat_is_nan,      NULL, 0);
	add_fn(vm, num_ctor, "isFinite",   js_nat_is_finite,   NULL, 0);
	add_fn(vm, num_ctor, "parseInt",   js_nat_parse_int,   NULL, 0);
	add_fn(vm, num_ctor, "parseFloat", js_nat_parse_float, NULL, 0);
	ipo_js_set_prop(vm, num_ctor, "NaN", ipo_js_num(NAN));
	ipo_js_set_prop(vm, num_ctor, "POSITIVE_INFINITY", ipo_js_num(INFINITY));
	ipo_js_set_prop(vm, num_ctor, "NEGATIVE_INFINITY", ipo_js_num(-INFINITY));
	ipo_js_set_prop(vm, g, "Number",  ipo_js_obj_val(num_ctor));
	add_fn(vm, g, "Boolean", js_bool_ctor, NULL, 1);

	/* RegExp */
	add_fn(vm, vm->regexp_proto, "test", js_re_test_exec, (void *)0, 0);
	add_fn(vm, vm->regexp_proto, "exec", js_re_test_exec, (void *)1, 0);
	ipo_js_obj_t *re_ctor = ipo_js_new_native_fn(vm, "RegExp", js_re_ctor, NULL, 1);
	ipo_js_set_prop(vm, re_ctor, "prototype", ipo_js_obj_val(vm->regexp_proto));
	ipo_js_set_prop(vm, g, "RegExp", ipo_js_obj_val(re_ctor));

	/* Math */
	ipo_js_obj_t *math_o = ipo_js_new_object(vm, vm->obj_proto);
	ipo_js_set_prop(vm, math_o, "PI",    ipo_js_num(3.14159265358979323846));
	ipo_js_set_prop(vm, math_o, "E",     ipo_js_num(2.71828182845904523536));
	ipo_js_set_prop(vm, math_o, "LN2",   ipo_js_num(0.69314718055994530942));
	ipo_js_set_prop(vm, math_o, "LN10",  ipo_js_num(2.30258509299404568402));
	ipo_js_set_prop(vm, math_o, "SQRT2", ipo_js_num(1.41421356237309504880));
	add_fn(vm, math_o, "abs",    js_math_1,      (void *)0, 0);
	add_fn(vm, math_o, "floor",  js_math_1,      (void *)1, 0);
	add_fn(vm, math_o, "ceil",   js_math_1,      (void *)2, 0);
	add_fn(vm, math_o, "round",  js_math_1,      (void *)3, 0);
	add_fn(vm, math_o, "trunc",  js_math_1,      (void *)4, 0);
	add_fn(vm, math_o, "sqrt",   js_math_1,      (void *)5, 0);
	add_fn(vm, math_o, "sin",    js_math_1,      (void *)6, 0);
	add_fn(vm, math_o, "cos",    js_math_1,      (void *)7, 0);
	add_fn(vm, math_o, "tan",    js_math_1,      (void *)8, 0);
	add_fn(vm, math_o, "log",    js_math_1,      (void *)9, 0);
	add_fn(vm, math_o, "exp",    js_math_1,      (void *)10, 0);
	add_fn(vm, math_o, "sign",   js_math_1,      (void *)11, 0);
	add_fn(vm, math_o, "log10",  js_math_1,      (void *)12, 0);
	add_fn(vm, math_o, "log2",   js_math_1,      (void *)13, 0);
	add_fn(vm, math_o, "atan",   js_math_1,      (void *)14, 0);
	add_fn(vm, math_o, "pow",    js_math_pow,    NULL, 0);
	add_fn(vm, math_o, "atan2",  js_math_atan2,  NULL, 0);
	add_fn(vm, math_o, "min",    js_math_minmax, (void *)0, 0);
	add_fn(vm, math_o, "max",    js_math_minmax, (void *)1, 0);
	add_fn(vm, math_o, "random", js_math_random, NULL, 0);
	ipo_js_set_prop(vm, g, "Math", ipo_js_obj_val(math_o));

	/* Date */
	add_fn(vm, vm->date_proto, "getTime",         js_date_get, (void *)0, 0);
	add_fn(vm, vm->date_proto, "valueOf",         js_date_get, (void *)0, 0);
	add_fn(vm, vm->date_proto, "getFullYear",     js_date_get, (void *)1, 0);
	add_fn(vm, vm->date_proto, "getMonth",        js_date_get, (void *)2, 0);
	add_fn(vm, vm->date_proto, "getDate",         js_date_get, (void *)3, 0);
	add_fn(vm, vm->date_proto, "getDay",          js_date_get, (void *)4, 0);
	add_fn(vm, vm->date_proto, "getHours",        js_date_get, (void *)5, 0);
	add_fn(vm, vm->date_proto, "getMinutes",      js_date_get, (void *)6, 0);
	add_fn(vm, vm->date_proto, "getSeconds",      js_date_get, (void *)7, 0);
	add_fn(vm, vm->date_proto, "getMilliseconds", js_date_get, (void *)8, 0);
	add_fn(vm, vm->date_proto, "toISOString",     js_date_get, (void *)9, 0);
	add_fn(vm, vm->date_proto, "toString",        js_date_get, (void *)9, 0);
	ipo_js_obj_t *date_ctor = ipo_js_new_native_fn(vm, "Date", js_date_ctor, NULL, 1);
	ipo_js_set_prop(vm, date_ctor, "prototype", ipo_js_obj_val(vm->date_proto));
	add_fn(vm, date_ctor, "now", js_date_now, NULL, 0);
	ipo_js_set_prop(vm, g, "Date", ipo_js_obj_val(date_ctor));

	/* JSON */
	ipo_js_obj_t *json_o = ipo_js_new_object(vm, vm->obj_proto);
	add_fn(vm, json_o, "parse",     js_json_parse,         NULL, 0);
	add_fn(vm, json_o, "stringify", js_json_stringify_nat, NULL, 0);
	ipo_js_set_prop(vm, g, "JSON", ipo_js_obj_val(json_o));

	/* Errors */
	add_fn(vm, vm->err_proto, "toString", js_obj_to_str, NULL, 0);
	add_fn(vm, g, "Error",          js_err_ctor, (void *)"Error", 1);
	add_fn(vm, g, "TypeError",      js_err_ctor, (void *)"TypeError", 1);
	add_fn(vm, g, "ReferenceError", js_err_ctor, (void *)"ReferenceError", 1);
	add_fn(vm, g, "SyntaxError",    js_err_ctor, (void *)"SyntaxError", 1);
	add_fn(vm, g, "RangeError",     js_err_ctor, (void *)"RangeError", 1);

	/* Promise */
	add_fn(vm, vm->promise_proto, "then",  js_prom_then, (void *)0, 0);
	add_fn(vm, vm->promise_proto, "catch", js_prom_then, (void *)1, 0);
	ipo_js_obj_t *prom_ctor = ipo_js_new_native_fn(vm, "Promise", js_prom_ctor, NULL, 1);
	ipo_js_set_prop(vm, prom_ctor, "prototype", ipo_js_obj_val(vm->promise_proto));
	add_fn(vm, prom_ctor, "resolve", js_prom_static_settle, (void *)1, 0);
	add_fn(vm, prom_ctor, "reject",  js_prom_static_settle, (void *)2, 0);
	ipo_js_set_prop(vm, g, "Promise", ipo_js_obj_val(prom_ctor));

	/* console */
	ipo_js_obj_t *con_o = ipo_js_new_object(vm, vm->obj_proto);
	add_fn(vm, con_o, "log",   js_nat_console_log, (void *)0, 0);
	add_fn(vm, con_o, "info",  js_nat_console_log, (void *)0, 0);
	add_fn(vm, con_o, "debug", js_nat_console_log, (void *)0, 0);
	add_fn(vm, con_o, "warn",  js_nat_console_log, (void *)1, 0);
	add_fn(vm, con_o, "error", js_nat_console_log, (void *)2, 0);
	ipo_js_set_prop(vm, g, "console", ipo_js_obj_val(con_o));

	return vm;
}

void ipo_js_vm_free(ipo_js_vm_t *vm) {
	if (!vm) return;
	for (ipo_js_obj_t *o = vm->gc_objs; o; ) {
		ipo_js_obj_t *nx = o->gc_next;
		if (o->host_finalize) o->host_finalize(vm, o);
		free(o->props);
		if (o->kind == JS_OBJ_ARRAY) free(o->u.arr.items);
		else if (o->kind == JS_OBJ_FUNCTION) free(o->u.fn.bound_args);
		else if (o->kind == JS_OBJ_NATIVE_FN) free(o->u.nat.bound_args);
		else if (o->kind == JS_OBJ_PROMISE) {
			free(o->u.prom.then_fns);
			free(o->u.prom.catch_fns);
			free(o->u.prom.then_promises);
		}
		free(o);
		o = nx;
	}
	for (ipo_js_env_t *e = vm->gc_envs; e; ) {
		ipo_js_env_t *nx = e->gc_next;
		free(e->vars);
		free(e);
		e = nx;
	}
	for (ipo_js_str_t *s = vm->gc_strs; s; ) {
		ipo_js_str_t *nx = s->gc_next;
		free(s);
		s = nx;
	}
	for (size_t i = 0; i < vm->ast_pool_len; i++) free_ast_pool(vm->ast_pools[i]);
	free(vm->ast_pools);
	free(vm->root_stack);
	free(vm->mark_stack);
	free(vm->frames);
	free(vm->microtasks);
	free(vm->sig_label);
	free(vm);
}

void *ipo_js_vm_host_ctx(ipo_js_vm_t *vm) { return vm ? vm->host_ctx : NULL; }
void ipo_js_vm_set_limits(ipo_js_vm_t *vm, int d, int s) { if (vm) { vm->max_depth = d; vm->max_steps = s; } }
void ipo_js_vm_set_console_hook(ipo_js_vm_t *vm, ipo_js_console_fn fn) { if (vm) vm->console_hook = fn; }
ipo_js_obj_t *ipo_js_global(ipo_js_vm_t *vm) { return vm ? vm->global_obj : NULL; }
ipo_js_obj_t *ipo_js_object_proto(ipo_js_vm_t *vm) { return vm ? vm->obj_proto : NULL; }
ipo_js_obj_t *ipo_js_array_proto(ipo_js_vm_t *vm) { return vm ? vm->arr_proto : NULL; }

int ipo_js_eval(ipo_js_vm_t *vm, const char *code, size_t len, const char *filename,
                ipo_js_val_t *out_val) {
	if (!vm) return -1;
	ipo_js_clear_exception(vm);
	vm->sig = SIG_NONE;
	if (vm->call_depth == 0) vm->steps = 0;

	/* Strip optional HTML comment wrappers at start/end of <script> blocks: <!-- ... --> */
	while (len > 0 && isspace((unsigned char)*code)) { code++; len--; }
	if (len >= 4 && memcmp(code, "<!--", 4) == 0) {
		const char *nl = (const char *)memchr(code, '\n', len);
		if (nl) { len -= (size_t)(nl + 1 - code); code = nl + 1; }
	}
	if (len >= 3) {
		const char *tail = code + len;
		while (tail > code && isspace((unsigned char)tail[-1])) tail--;
		if (tail - code >= 3 && memcmp(tail - 3, "-->", 3) == 0) {
			/* check if on its own line or preceded by // */
			const char *ls = tail - 3;
			while (ls > code && (ls[-1] == ' ' || ls[-1] == '\t' || ls[-1] == '/')) ls--;
			if (ls == code || ls[-1] == '\n' || ls[-1] == '\r') len = (size_t)(ls - code);
		}
	}

	ipo_js_ast_t *root = NULL;
	js_ast_pool_t *pool = parse_program(vm, code, len, filename, &root);
	if (!pool) {
		if (out_val) *out_val = ipo_js_undef();
		return -1;
	}
	vm->ast_pools = (js_ast_pool_t **)xrealloc(vm->ast_pools, (vm->ast_pool_len + 1) * sizeof(js_ast_pool_t *));
	vm->ast_pools[vm->ast_pool_len++] = pool;

	hoist_block(vm, vm->global_env, vm->global_env, root, 1);
	ipo_js_val_t res = vm_exec_stmt(vm, vm->global_env, root);
	if (vm->sig == SIG_RETURN) {
		res = vm->sig_val;
		vm->sig = SIG_NONE;
		vm->sig_val = ipo_js_undef();
	}
	vm_drain_microtasks(vm);
	if (out_val) *out_val = res;
	if (vm->has_exc) return -1;
	gc_maybe_collect(vm);
	return 0;
}

int ipo_js_call(ipo_js_vm_t *vm, ipo_js_val_t fn, ipo_js_val_t this_val,
                int argc, const ipo_js_val_t *argv, ipo_js_val_t *out_val) {
	if (!vm) return -1;
	ipo_js_clear_exception(vm);
	vm->sig = SIG_NONE;
	if (vm->call_depth == 0) vm->steps = 0;
	ipo_js_val_t r = vm_call_obj(vm, fn, this_val, argc, argv, NULL);
	vm_drain_microtasks(vm);
	if (out_val) *out_val = r;
	if (vm->has_exc) return -1;
	gc_maybe_collect(vm);
	return 0;
}
