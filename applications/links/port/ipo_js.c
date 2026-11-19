/*
 * Links Browser <-> IPO JS VM & Live HTML/CSS DOM Bridge
 * Implements Links #ifdef JS entrypoints (struct.h / ipret.h / links.h)
 * and browser Web APIs (window, document, DOM tree, style, classList,
 * events, form sync, alert/confirm/prompt, setTimeout/setInterval, fetch/XHR).
 */

#include "cfg.h"

#ifdef JS

#include "links.h"
#include "struct.h"
#include "ipret.h"
#include "builtin_keys.h"
#include "ipo_js_engine.h"
#include "ipo_gui.h"
#include "youtube_client.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

/* =========================================================================
 * Links Required Memory Accounting & Downcalls
 * ========================================================================= */

long js_zaflaknuto_pameti = 0;

void *js_mem_alloc(size_t size) {
	return malloc(size ? size : 1);
}

void js_mem_free(void *ptr) {
	free(ptr);
}

unsigned char *stracpy1(const unsigned char *src) {
	if (!src) src = (const unsigned char *)"";
	size_t len = strlen((const char *)src);
	unsigned char *dst = (unsigned char *)js_mem_alloc(len + 1);
	if (dst) memcpy(dst, src, len + 1);
	return dst;
}

void js_spec_vykill_timer(js_context *ctx, int type) {
	(void)type;
	if (!ctx) return;
	if (ctx->exec_timer) {
		kill_timer((struct timer *)ctx->exec_timer);
		ctx->exec_timer = NULL;
	}
}

void js_downcall_vezmi_true(void *context) { (void)context; }
void js_downcall_vezmi_false(void *context) { (void)context; }
void js_downcall_vezmi_null(void *context) { (void)context; }
void js_downcall_quiet_game_over(void *context) { (void)context; }
void js_downcall_vezmi_string(void *context, unsigned char *str) {
	(void)context;
	if (str) mem_free(str);
}

/* =========================================================================
 * DOM Representation & Parser / Serializer
 * ========================================================================= */

typedef enum {
	DOM_DOCUMENT = 0,
	DOM_ELEMENT,
	DOM_TEXT,
	DOM_COMMENT
} dom_kind_t;

typedef struct dom_attr {
	char *name;
	char *value;
} dom_attr_t;

typedef struct dom_listener {
	char *type;
	ipo_js_val_t fn;
	struct dom_listener *next;
} dom_listener_t;

typedef struct dom_node {
	dom_kind_t kind;
	int elem_id;
	char *tag; /* lowercased for DOM_ELEMENT */
	dom_attr_t *attrs;
	size_t attr_len;
	size_t attr_cap;
	char *text; /* for DOM_TEXT / DOM_COMMENT / doctype */
	int attr_dirty_form;

	struct dom_node *parent;
	struct dom_node *first_child;
	struct dom_node *last_child;
	struct dom_node *next_sibling;
	struct dom_node *prev_sibling;

	ipo_js_obj_t *js_wrapper;
	ipo_js_obj_t *style_wrapper;
	ipo_js_obj_t *classlist_wrapper;
	ipo_js_obj_t *dataset_wrapper;
	dom_listener_t *listeners;
	int script_executed;
} dom_node_t;

typedef struct js_timer_entry {
	int id;
	int is_interval;
	int interval_ms;
	int cancelled;
	struct timer *links_timer;
	ipo_js_val_t fn;
	ipo_js_val_t *args;
	int argc;
	char *code_str;
	struct ipo_js_runtime *rt;
	struct js_timer_entry *next;
} js_timer_entry_t;

typedef struct js_kv_item {
	char *key;
	char *val;
	struct js_kv_item *next;
} js_kv_item_t;

typedef struct ipo_js_runtime {
	js_context *ctx;
	ipo_js_vm_t *vm;
	dom_node_t *doc;
	int next_elem_id;
	int dom_dirty;
	int need_reparse_dom;
	int active_script_ord;
	int destroyed;

	void (*pending_done_cb)(void *);
	unsigned char *pending_code;
	int pending_len;

	int next_timer_id;
	js_timer_entry_t *timers;
	dom_listener_t *win_listeners;
	js_kv_item_t *local_storage;
	js_kv_item_t *session_storage;
	double start_ms;

	ipo_js_obj_t *doc_obj;
	ipo_js_obj_t *loc_obj;
} ipo_js_runtime_t;

#define HTAG_DOCUMENT  1
#define HTAG_ELEMENT   2
#define HTAG_STYLE     3
#define HTAG_CLASSLIST 4
#define HTAG_DATASET   5
#define HTAG_LOCATION  6
#define HTAG_HISTORY   7
#define HTAG_STORAGE   8

/* String / buffer helpers */
typedef struct {
	char *data;
	size_t len;
	size_t cap;
} dbuf_t;

static void db_init(dbuf_t *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void db_free(dbuf_t *b) { free(b->data); b->data = NULL; b->len = 0; b->cap = 0; }
static void db_add(dbuf_t *b, const char *s, size_t n) {
	if (!s || !n) return;
	if (b->len + n + 1 > b->cap) {
		size_t nc = b->cap ? b->cap : 128;
		while (nc < b->len + n + 1) nc *= 2;
		char *nd = (char *)realloc(b->data, nc);
		if (!nd) return;
		b->data = nd;
		b->cap = nc;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}
static void db_adds(dbuf_t *b, const char *s) { if (s) db_add(b, s, strlen(s)); }
static void db_addc(dbuf_t *b, char c) { db_add(b, &c, 1); }

static char *xdup(const char *s) {
	if (!s) return strdup("");
	size_t n = strlen(s);
	char *r = (char *)malloc(n + 1);
	if (r) { memcpy(r, s, n); r[n] = '\0'; }
	return r;
}

static char *xdupn_lower(const char *s, size_t n) {
	char *r = (char *)malloc(n + 1);
	if (r) {
		for (size_t i = 0; i < n; i++) r[i] = (char)tolower((unsigned char)s[i]);
		r[n] = '\0';
	}
	return r;
}

static int ci_eq(const char *a, const char *b) {
	if (!a || !b) return 0;
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
		a++; b++;
	}
	return *a == *b;
}

/* =========================================================================
 * DOM Node Operations
 * ========================================================================= */

static dom_node_t *dom_alloc_node(ipo_js_runtime_t *rt, dom_kind_t k) {
	dom_node_t *n = (dom_node_t *)calloc(1, sizeof(dom_node_t));
	if (!n) return NULL;
	n->kind = k;
	if (k == DOM_ELEMENT && rt) n->elem_id = ++rt->next_elem_id;
	return n;
}

static void dom_free_listeners(ipo_js_runtime_t *rt, dom_listener_t *l) {
	while (l) {
		dom_listener_t *nx = l->next;
		if (l->fn.type == JS_T_OBJECT && l->fn.u.obj && rt && rt->vm) {
			ipo_js_gc_unpin(l->fn.u.obj);
		}
		free(l->type);
		free(l);
		l = nx;
	}
}

static void dom_free_subtree(ipo_js_runtime_t *rt, dom_node_t *root) {
	if (!root) return;
	size_t cap = 64, sp = 0;
	dom_node_t **stk = (dom_node_t **)malloc(cap * sizeof(dom_node_t *));
	if (!stk) return;
	stk[sp++] = root;
	while (sp > 0) {
		dom_node_t *n = stk[--sp];
		for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
			if (sp >= cap) {
				cap *= 2;
				dom_node_t **ns = (dom_node_t **)realloc(stk, cap * sizeof(dom_node_t *));
				if (!ns) break;
				stk = ns;
			}
			stk[sp++] = c;
		}
		if (n->js_wrapper) { n->js_wrapper->host_ptr = NULL; ipo_js_gc_unpin(n->js_wrapper); }
		if (n->style_wrapper) { n->style_wrapper->host_ptr = NULL; }
		if (n->classlist_wrapper) { n->classlist_wrapper->host_ptr = NULL; }
		if (n->dataset_wrapper) { n->dataset_wrapper->host_ptr = NULL; }
		dom_free_listeners(rt, n->listeners);
		for (size_t i = 0; i < n->attr_len; i++) {
			free(n->attrs[i].name);
			free(n->attrs[i].value);
		}
		free(n->attrs);
		free(n->tag);
		free(n->text);
		free(n);
	}
	free(stk);
}

static void dom_detach(dom_node_t *c) {
	if (!c || !c->parent) return;
	dom_node_t *p = c->parent;
	if (c->prev_sibling) c->prev_sibling->next_sibling = c->next_sibling;
	else p->first_child = c->next_sibling;
	if (c->next_sibling) c->next_sibling->prev_sibling = c->prev_sibling;
	else p->last_child = c->prev_sibling;
	c->parent = NULL;
	c->prev_sibling = NULL;
	c->next_sibling = NULL;
}

static void dom_append_child(dom_node_t *p, dom_node_t *c) {
	if (!p || !c || c == p) return;
	dom_detach(c);
	c->parent = p;
	c->prev_sibling = p->last_child;
	c->next_sibling = NULL;
	if (p->last_child) p->last_child->next_sibling = c;
	else p->first_child = c;
	p->last_child = c;
}

static void dom_insert_before(dom_node_t *p, dom_node_t *c, dom_node_t *ref) {
	if (!p || !c || c == p) return;
	if (!ref || ref->parent != p) { dom_append_child(p, c); return; }
	dom_detach(c);
	c->parent = p;
	c->next_sibling = ref;
	c->prev_sibling = ref->prev_sibling;
	if (ref->prev_sibling) ref->prev_sibling->next_sibling = c;
	else p->first_child = c;
	ref->prev_sibling = c;
}

static const char *dom_get_attr(const dom_node_t *n, const char *name) {
	if (!n || !name) return NULL;
	for (size_t i = 0; i < n->attr_len; i++) {
		if (ci_eq(n->attrs[i].name, name)) return n->attrs[i].value;
	}
	return NULL;
}

static void dom_set_attr(dom_node_t *n, const char *name, const char *val) {
	if (!n || !name) return;
	for (size_t i = 0; i < n->attr_len; i++) {
		if (ci_eq(n->attrs[i].name, name)) {
			free(n->attrs[i].value);
			n->attrs[i].value = xdup(val ? val : "");
			return;
		}
	}
	if (n->attr_len >= n->attr_cap) {
		size_t nc = n->attr_cap ? n->attr_cap * 2 : 4;
		dom_attr_t *na = (dom_attr_t *)realloc(n->attrs, nc * sizeof(dom_attr_t));
		if (!na) return;
		n->attrs = na;
		n->attr_cap = nc;
	}
	n->attrs[n->attr_len].name = xdupn_lower(name, strlen(name));
	n->attrs[n->attr_len].value = xdup(val ? val : "");
	n->attr_len++;
}

static void dom_remove_attr(dom_node_t *n, const char *name) {
	if (!n || !name) return;
	for (size_t i = 0; i < n->attr_len; i++) {
		if (ci_eq(n->attrs[i].name, name)) {
			free(n->attrs[i].name);
			free(n->attrs[i].value);
			for (size_t j = i + 1; j < n->attr_len; j++) n->attrs[j - 1] = n->attrs[j];
			n->attr_len--;
			return;
		}
	}
}

static int is_void_tag(const char *t) {
	static const char * const v[] = {
		"area", "base", "br", "col", "embed", "hr", "img", "input",
		"link", "meta", "param", "source", "track", "wbr",
		"path", "circle", "rect", "line", "polygon", "polyline", "ellipse", "stop", "use",
		NULL
	};
	for (int i = 0; v[i]; i++) if (strcmp(t, v[i]) == 0) return 1;
	return 0;
}

static void decode_html_entities(const char *s, size_t len, dbuf_t *out) {
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '&') {
			const char *semi = NULL;
			for (size_t k = 1; k <= 10 && i + k < len; k++) {
				if (s[i + k] == ';') { semi = s + i + k; break; }
				if (!isalnum((unsigned char)s[i + k]) && s[i + k] != '#') break;
			}
			if (semi) {
				size_t el = (size_t)(semi - (s + i + 1));
				const char *e = s + i + 1;
				if (el == 3 && memcmp(e, "amp", 3) == 0) { db_addc(out, '&'); i += el + 1; continue; }
				if (el == 2 && memcmp(e, "lt", 2) == 0)  { db_addc(out, '<'); i += el + 1; continue; }
				if (el == 2 && memcmp(e, "gt", 2) == 0)  { db_addc(out, '>'); i += el + 1; continue; }
				if (el == 4 && memcmp(e, "quot", 4) == 0){ db_addc(out, '"'); i += el + 1; continue; }
				if (el == 4 && memcmp(e, "apos", 4) == 0){ db_addc(out, '\''); i += el + 1; continue; }
				if (el == 4 && memcmp(e, "nbsp", 4) == 0){ db_addc(out, ' '); i += el + 1; continue; }
				if (el >= 2 && e[0] == '#') {
					unsigned long cp = 0;
					if (e[1] == 'x' || e[1] == 'X') cp = strtoul(e + 2, NULL, 16);
					else cp = strtoul(e + 1, NULL, 10);
					if (cp > 0 && cp < 0x80) db_addc(out, (char)cp);
					else if (cp >= 0x80 && cp <= 0x7FF) {
						db_addc(out, (char)(0xC0 | (cp >> 6)));
						db_addc(out, (char)(0x80 | (cp & 0x3F)));
					}
					i += el + 1;
					continue;
				}
			}
		}
		db_addc(out, s[i]);
	}
}

static void parse_html_into(ipo_js_runtime_t *rt, dom_node_t *root, const char *html, size_t len) {
	dom_node_t *cur = root;
	int depth = 0;
	const char *p = html ? html : "";
	const char *end = p + len;

	while (p < end) {
		const char *lt = (const char *)memchr(p, '<', (size_t)(end - p));
		if (!lt) {
			if (p < end) {
				dbuf_t tb; db_init(&tb);
				decode_html_entities(p, (size_t)(end - p), &tb);
				dom_node_t *tn = dom_alloc_node(rt, DOM_TEXT);
				tn->text = tb.data ? tb.data : xdup("");
				dom_append_child(cur, tn);
			}
			break;
		}
		if (lt > p) {
			dbuf_t tb; db_init(&tb);
			decode_html_entities(p, (size_t)(lt - p), &tb);
			dom_node_t *tn = dom_alloc_node(rt, DOM_TEXT);
			tn->text = tb.data ? tb.data : xdup("");
			dom_append_child(cur, tn);
		}
		p = lt;
		/* Comment <!-- ... --> */
		if (p + 4 <= end && memcmp(p, "<!--", 4) == 0) {
			const char *ce = NULL;
			for (const char *q = p + 4; q + 3 <= end; q++) {
				if (memcmp(q, "-->", 3) == 0) { ce = q; break; }
			}
			dom_node_t *cn = dom_alloc_node(rt, DOM_COMMENT);
			if (ce) {
				size_t clen = (size_t)(ce - (p + 4));
				cn->text = (char *)malloc(clen + 1);
				memcpy(cn->text, p + 4, clen); cn->text[clen] = '\0';
				p = ce + 3;
			} else {
				cn->text = xdup(p + 4);
				p = end;
			}
			dom_append_child(cur, cn);
			continue;
		}
		/* <!DOCTYPE ...> or <?...?> */
		if (p + 2 <= end && (p[1] == '!' || p[1] == '?')) {
			const char *gt = (const char *)memchr(p, '>', (size_t)(end - p));
			p = gt ? gt + 1 : end;
			continue;
		}
		/* Closing tag </tag> */
		if (p + 2 <= end && p[1] == '/') {
			p += 2;
			const char *ts = p;
			while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
			char *tname = xdupn_lower(ts, (size_t)(p - ts));
			const char *gt = (const char *)memchr(p, '>', (size_t)(end - p));
			p = gt ? gt + 1 : end;
			/* Walk up to matching tag */
			dom_node_t *m = cur;
			int steps = 0;
			while (m && m != root) {
				steps++;
				if (m->kind == DOM_ELEMENT && strcmp(m->tag, tname) == 0) break;
				m = m->parent;
			}
			if (m && m != root && m->parent) {
				cur = m->parent;
				depth = (depth >= steps) ? (depth - steps) : 0;
			}
			free(tname);
			continue;
		}
		/* Opening tag <tag ...> */
		p++;
		if (p >= end || !isalpha((unsigned char)*p)) {
			dom_node_t *tn = dom_alloc_node(rt, DOM_TEXT);
			tn->text = xdup("<");
			dom_append_child(cur, tn);
			continue;
		}
		const char *ts = p;
		while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ':')) p++;
		char *tname = xdupn_lower(ts, (size_t)(p - ts));
		dom_node_t *el = dom_alloc_node(rt, DOM_ELEMENT);
		el->tag = tname;

		int self_close = 0;
		while (p < end) {
			while (p < end && isspace((unsigned char)*p)) p++;
			if (p >= end) break;
			if (*p == '>') { p++; break; }
			if (*p == '/' && p + 1 < end && p[1] == '>') { self_close = 1; p += 2; break; }
			if (*p == '/') { p++; continue; }
			const char *as = p;
			while (p < end && !isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '/') p++;
			if (p == as) { p++; continue; }
			char *aname = xdupn_lower(as, (size_t)(p - as));
			while (p < end && isspace((unsigned char)*p)) p++;
			dbuf_t av; db_init(&av);
			if (p < end && *p == '=') {
				p++;
				while (p < end && isspace((unsigned char)*p)) p++;
				if (p < end && (*p == '"' || *p == '\'')) {
					char q = *p++;
					const char *vs = p;
					while (p < end && *p != q) p++;
					decode_html_entities(vs, (size_t)(p - vs), &av);
					if (p < end && *p == q) p++;
				} else {
					const char *vs = p;
					while (p < end && !isspace((unsigned char)*p) && *p != '>') p++;
					decode_html_entities(vs, (size_t)(p - vs), &av);
				}
			}
			dom_set_attr(el, aname, av.data ? av.data : "");
			db_free(&av);
			free(aname);
		}

		/* Implicit optional closing for common HTML lists/paragraphs/options */
		if (cur != root && cur->tag &&
		    ((strcmp(tname, "li") == 0 && strcmp(cur->tag, "li") == 0) ||
		     (strcmp(tname, "option") == 0 && strcmp(cur->tag, "option") == 0) ||
		     (strcmp(tname, "dt") == 0 && (strcmp(cur->tag, "dt") == 0 || strcmp(cur->tag, "dd") == 0)) ||
		     (strcmp(tname, "dd") == 0 && (strcmp(cur->tag, "dt") == 0 || strcmp(cur->tag, "dd") == 0)) ||
		     (strcmp(tname, "tr") == 0 && (strcmp(cur->tag, "tr") == 0 || strcmp(cur->tag, "td") == 0 || strcmp(cur->tag, "th") == 0)) ||
		     ((strcmp(tname, "td") == 0 || strcmp(tname, "th") == 0) && (strcmp(cur->tag, "td") == 0 || strcmp(cur->tag, "th") == 0)) ||
		     (strcmp(tname, "p") == 0 && strcmp(cur->tag, "p") == 0))) {
			cur = cur->parent ? cur->parent : root;
			if (depth > 0) depth--;
		}
		dom_append_child(cur, el);

		if (strcmp(tname, "script") == 0 || strcmp(tname, "style") == 0) {
			/* Scan raw text until </script> or </style> */
			char ctag[16];
			snprintf(ctag, sizeof(ctag), "</%s", tname);
			size_t ctl = strlen(ctag);
			const char *rs = p;
			const char *re = end;
			for (const char *q = p; q + ctl < end; q++) {
				if (q[0] == '<' && q[1] == '/' && strncasecmp(q + 2, tname, strlen(tname)) == 0) {
					const char *gt = (const char *)memchr(q + 2 + strlen(tname), '>', (size_t)(end - (q + 2 + strlen(tname))));
					re = q;
					p = gt ? gt + 1 : end;
					break;
				}
			}
			if (re == end) p = end;
			if (re > rs) {
				dom_node_t *tn = dom_alloc_node(rt, DOM_TEXT);
				size_t rl = (size_t)(re - rs);
				tn->text = (char *)malloc(rl + 1);
				memcpy(tn->text, rs, rl); tn->text[rl] = '\0';
				dom_append_child(el, tn);
			}
		} else if (!self_close && !is_void_tag(tname) && depth < 128) {
			cur = el;
			depth++;
		}
	}
}

static void db_add_attr_escaped(dbuf_t *b, const char *s) {
	if (!s) return;
	for (const char *p = s; *p; p++) {
		if (*p == '"') db_adds(b, "&quot;");
		else if (*p == '&') db_adds(b, "&amp;");
		else if (*p == '<') db_adds(b, "&lt;");
		else db_addc(b, *p);
	}
}

static void db_add_text_escaped(dbuf_t *b, const char *s) {
	if (!s) return;
	for (const char *p = s; *p; p++) {
		if (*p == '&') db_adds(b, "&amp;");
		else if (*p == '<') db_adds(b, "&lt;");
		else if (*p == '>') db_adds(b, "&gt;");
		else db_addc(b, *p);
	}
}

static int node_has_listener(const dom_node_t *n, const char *type) {
	for (dom_listener_t *l = n->listeners; l; l = l->next) {
		if (strcmp(l->type, type) == 0) return 1;
	}
	return 0;
}

static void dom_serialize_open_tag(const dom_node_t *n, dbuf_t *b) {
	const char *tag = n->tag ? n->tag : "div";
	db_addc(b, '<');
	db_adds(b, tag);

	int has_onclick = 0, has_onchange = 0, has_onsubmit = 0;
	for (size_t i = 0; i < n->attr_len; i++) {
		const char *an = n->attrs[i].name;
		const char *av = n->attrs[i].value ? n->attrs[i].value : "";
		if (strcmp(an, "onclick") == 0)  has_onclick = 1;
		if (strcmp(an, "onchange") == 0) has_onchange = 1;
		if (strcmp(an, "onsubmit") == 0) has_onsubmit = 1;

		db_addc(b, ' ');
		db_adds(b, an);
		if (strcmp(an, "onclick") == 0 && node_has_listener(n, "click") &&
		    strstr(av, "__ipoDispatch(") == NULL) {
			char extra[96];
			snprintf(extra, sizeof(extra), "__ipoDispatch(%d,'click');", n->elem_id);
			db_adds(b, "=\"");
			db_add_attr_escaped(b, extra);
			db_add_attr_escaped(b, av);
			db_addc(b, '"');
		} else if (strcmp(an, "onchange") == 0 && node_has_listener(n, "change") &&
		           strstr(av, "__ipoDispatch(") == NULL) {
			char extra[96];
			snprintf(extra, sizeof(extra), "__ipoDispatch(%d,'change');", n->elem_id);
			db_adds(b, "=\"");
			db_add_attr_escaped(b, extra);
			db_add_attr_escaped(b, av);
			db_addc(b, '"');
		} else {
			db_adds(b, "=\"");
			db_add_attr_escaped(b, av);
			db_addc(b, '"');
		}
	}

	if (!has_onclick && node_has_listener(n, "click")) {
		char attr[96];
		snprintf(attr, sizeof(attr), " onclick=\"return __ipoDispatch(%d,'click')\"", n->elem_id);
		db_adds(b, attr);
	}
	if (!has_onchange && node_has_listener(n, "change")) {
		char attr[96];
		snprintf(attr, sizeof(attr), " onchange=\"return __ipoDispatch(%d,'change')\"", n->elem_id);
		db_adds(b, attr);
	}
	if (!has_onsubmit && node_has_listener(n, "submit")) {
		char attr[96];
		snprintf(attr, sizeof(attr), " onsubmit=\"return __ipoDispatch(%d,'submit')\"", n->elem_id);
		db_adds(b, attr);
	}
	db_addc(b, '>');
}

static void dom_serialize_node(const dom_node_t *root, dbuf_t *b, int inner_only,
                               int track_script_ord, int *cur_script_ord, size_t *out_script_end) {
	if (!root) return;
	if (root->kind == DOM_TEXT) {
		if (root->parent && root->parent->tag &&
		    (strcmp(root->parent->tag, "script") == 0 || strcmp(root->parent->tag, "style") == 0)) {
			db_adds(b, root->text ? root->text : "");
		} else {
			db_add_text_escaped(b, root->text ? root->text : "");
		}
		return;
	}
	if (root->kind == DOM_COMMENT) {
		db_adds(b, "<!--");
		db_adds(b, root->text ? root->text : "");
		db_adds(b, "-->");
		return;
	}

	typedef struct {
		const dom_node_t *node;
		dom_node_t *next_child;
		int emit_close;
	} ser_frame_t;

	size_t cap = 64, sp = 0;
	ser_frame_t *stk = (ser_frame_t *)malloc(cap * sizeof(ser_frame_t));
	if (!stk) return;

	if (root->kind == DOM_DOCUMENT || inner_only) {
		stk[sp++] = (ser_frame_t){ root, root->first_child, 0 };
	} else {
		const char *tag = root->tag ? root->tag : "div";
		dom_serialize_open_tag(root, b);
		if (is_void_tag(tag)) { free(stk); return; }
		stk[sp++] = (ser_frame_t){ root, root->first_child, 1 };
	}

	while (sp > 0) {
		ser_frame_t *f = &stk[sp - 1];
		dom_node_t *c = f->next_child;
		if (!c) {
			if (f->emit_close) {
				const char *tag = f->node->tag ? f->node->tag : "div";
				db_adds(b, "</");
				db_adds(b, tag);
				db_addc(b, '>');
				if (strcmp(tag, "script") == 0 && cur_script_ord && out_script_end) {
					(*cur_script_ord)++;
					if (*cur_script_ord == track_script_ord) *out_script_end = b->len;
				}
			}
			sp--;
			continue;
		}
		f->next_child = c->next_sibling;
		if (c->kind == DOM_TEXT) {
			if (c->parent && c->parent->tag &&
			    (strcmp(c->parent->tag, "script") == 0 || strcmp(c->parent->tag, "style") == 0)) {
				db_adds(b, c->text ? c->text : "");
			} else {
				db_add_text_escaped(b, c->text ? c->text : "");
			}
		} else if (c->kind == DOM_COMMENT) {
			db_adds(b, "<!--");
			db_adds(b, c->text ? c->text : "");
			db_adds(b, "-->");
		} else {
			const char *tag = c->tag ? c->tag : "div";
			dom_serialize_open_tag(c, b);
			if (!is_void_tag(tag)) {
				if (sp >= cap) {
					cap *= 2;
					ser_frame_t *ns = (ser_frame_t *)realloc(stk, cap * sizeof(ser_frame_t));
					if (!ns) break;
					stk = ns;
				}
				stk[sp++] = (ser_frame_t){ c, c->first_child, 1 };
			}
		}
	}
	free(stk);
}

static dom_node_t *dom_next_dfs(const dom_node_t *root, dom_node_t *cur) {
	if (!cur) return NULL;
	if (cur->first_child) return cur->first_child;
	while (cur && cur != root) {
		if (cur->next_sibling) return cur->next_sibling;
		cur = cur->parent;
	}
	return NULL;
}

static dom_node_t *dom_find_tag_rec(dom_node_t *root, const char *tag) {
	for (dom_node_t *n = root; n; n = dom_next_dfs(root, n)) {
		if (n->kind == DOM_ELEMENT && n->tag && strcmp(n->tag, tag) == 0) return n;
	}
	return NULL;
}

static dom_node_t *dom_find_by_id_rec(dom_node_t *root, const char *id) {
	for (dom_node_t *n = root; n; n = dom_next_dfs(root, n)) {
		if (n->kind == DOM_ELEMENT) {
			const char *v = dom_get_attr(n, "id");
			if (v && strcmp(v, id) == 0) return n;
		}
	}
	return NULL;
}

static dom_node_t *dom_find_by_elem_id_rec(dom_node_t *root, int eid) {
	for (dom_node_t *n = root; n; n = dom_next_dfs(root, n)) {
		if (n->kind == DOM_ELEMENT && n->elem_id == eid) return n;
	}
	return NULL;
}

static int dom_node_ordinal_in_controls(const dom_node_t *root, const dom_node_t *target, int *cnt) {
	for (dom_node_t *n = (dom_node_t *)root; n; n = dom_next_dfs(root, n)) {
		if (n->kind == DOM_ELEMENT && n->tag &&
		    (strcmp(n->tag, "input") == 0 || strcmp(n->tag, "textarea") == 0 ||
		     strcmp(n->tag, "select") == 0)) {
			if (n == target) return *cnt;
			(*cnt)++;
		}
	}
	return -1;
}

static struct form_control *match_form_control(ipo_js_runtime_t *rt, dom_node_t *n, long *out_js_id) {
	if (!rt || !rt->ctx || !rt->ctx->ptr || !n || n->kind != DOM_ELEMENT || !n->tag) return NULL;
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	if (!fd->f_data) return NULL;

	int want_input = (strcmp(n->tag, "input") == 0 || strcmp(n->tag, "textarea") == 0 ||
	                  strcmp(n->tag, "select") == 0);
	if (!want_input && strcmp(n->tag, "button") != 0) return NULL;

	const char *dname = dom_get_attr(n, "name");
	int dummy_cnt = 0;
	int ord = want_input ? dom_node_ordinal_in_controls(rt->doc, n, &dummy_cnt) : -1;

	struct form_control *fc = NULL, *best_ord = NULL;
	struct list_head *lfc = NULL;
	int idx = 0;
	foreachback(struct form_control, fc, lfc, fd->f_data->forms) {
		int is_field = (fc->type == FC_TEXT || fc->type == FC_PASSWORD ||
		                fc->type == FC_TEXTAREA || fc->type == FC_CHECKBOX ||
		                fc->type == FC_RADIO || fc->type == FC_SELECT ||
		                fc->type == FC_HIDDEN || fc->type == FC_FILE_UPLOAD);
		if (dname && *dname && fc->name && strcmp((const char *)fc->name, dname) == 0) {
			if (want_input && is_field) { best_ord = fc; break; }
			if (!want_input && !is_field) { best_ord = fc; break; }
		}
		if (want_input && is_field) {
			if (idx == ord) best_ord = fc;
			idx++;
		}
	}
	if (!best_ord) return NULL;
	if (out_js_id) {
		int t = JS_OBJ_T_TEXT;
		switch (best_ord->type) {
		case FC_TEXT:     t = JS_OBJ_T_TEXT; break;
		case FC_PASSWORD: t = JS_OBJ_T_PASSWORD; break;
		case FC_TEXTAREA: t = JS_OBJ_T_TEXTAREA; break;
		case FC_CHECKBOX: t = JS_OBJ_T_CHECKBOX; break;
		case FC_RADIO:    t = JS_OBJ_T_RADIO; break;
		case FC_SELECT:   t = JS_OBJ_T_SELECT; break;
		case FC_SUBMIT:   t = JS_OBJ_T_SUBMIT; break;
		case FC_RESET:    t = JS_OBJ_T_RESET; break;
		case FC_HIDDEN:   t = JS_OBJ_T_HIDDEN; break;
		case FC_BUTTON:   t = JS_OBJ_T_BUTTON; break;
		default:          t = JS_OBJ_T_TEXT; break;
		}
		*out_js_id = (best_ord->g_ctrl_num << JS_OBJ_MASK_SIZE) | t;
	}
	return best_ord;
}

static void sync_live_forms_into_dom(ipo_js_runtime_t *rt, dom_node_t *root) {
	for (dom_node_t *n = root; n; n = dom_next_dfs(root, n)) {
		if (n->kind == DOM_ELEMENT && !n->attr_dirty_form && n->tag) {
			long jid = 0;
			struct form_control *fc = match_form_control(rt, n, &jid);
			if (fc) {
				struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
				if (fc->type == FC_TEXT || fc->type == FC_PASSWORD) {
					unsigned char *v = js_upcall_get_form_element_value(fd, rt->ctx->js_id, jid);
					if (v) { dom_set_attr(n, "value", (const char *)v); mem_free(v); }
				} else if (fc->type == FC_TEXTAREA) {
					unsigned char *v = js_upcall_get_form_element_value(fd, rt->ctx->js_id, jid);
					if (v) {
						while (n->first_child) { dom_node_t *c = n->first_child; dom_detach(c); dom_free_subtree(rt, c); }
						dom_node_t *tn = dom_alloc_node(rt, DOM_TEXT);
						tn->text = xdup((const char *)v);
						dom_append_child(n, tn);
						mem_free(v);
					}
				} else if (fc->type == FC_CHECKBOX || fc->type == FC_RADIO) {
					int chk = js_upcall_get_checkbox_radio_checked(fd, rt->ctx->js_id, jid);
					if (chk) dom_set_attr(n, "checked", "checked");
					else dom_remove_attr(n, "checked");
				}
			}
		}
	}
}

static dom_node_t *dom_ensure_parsed(ipo_js_runtime_t *rt) {
	if (!rt || !rt->ctx || !rt->ctx->ptr) return NULL;
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	if (rt->doc && !rt->need_reparse_dom) return rt->doc;

	if (rt->doc) {
		dom_free_subtree(rt, rt->doc);
		rt->doc = NULL;
	}
	rt->need_reparse_dom = 0;
	rt->doc = dom_alloc_node(rt, DOM_DOCUMENT);

	unsigned char *ss = NULL;
	size_t slen = 0;
	const char *src = "<html><head></head><body></body></html>";
	if (jsint_get_source(fd, &ss, &slen) && ss) {
		src = (const char *)ss;
	} else if (fd->rq && get_file(fd->rq, &ss, &slen) == 0 && ss) {
		src = (const char *)ss;
	} else {
		slen = strlen(src);
	}
	parse_html_into(rt, rt->doc, src, slen);

	/* Ensure <html> and <body> exist for script convenience */
	dom_node_t *html_el = dom_find_tag_rec(rt->doc, "html");
	if (!html_el) {
		html_el = dom_alloc_node(rt, DOM_ELEMENT);
		html_el->tag = xdup("html");
		while (rt->doc->first_child) {
			dom_node_t *c = rt->doc->first_child;
			dom_detach(c);
			dom_append_child(html_el, c);
		}
		dom_append_child(rt->doc, html_el);
	}
	if (!dom_find_tag_rec(rt->doc, "head")) {
		dom_node_t *hd = dom_alloc_node(rt, DOM_ELEMENT);
		hd->tag = xdup("head");
		dom_insert_before(html_el, hd, html_el->first_child);
	}
	if (!dom_find_tag_rec(rt->doc, "body")) {
		dom_node_t *bd = dom_alloc_node(rt, DOM_ELEMENT);
		bd->tag = xdup("body");
		dom_append_child(html_el, bd);
	}
	return rt->doc;
}

static void dom_commit_to_links(ipo_js_runtime_t *rt, int flush_now) {
	if (!rt || !rt->ctx || !rt->ctx->ptr || !rt->dom_dirty) return;
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	if (!fd->js) return;

	dom_node_t *doc = dom_ensure_parsed(rt);
	sync_live_forms_into_dom(rt, doc);

	dbuf_t b; db_init(&b);
	int cur_ord = 0;
	size_t script_end = 0;
	int cur_wp = jsint_get_active_write_pos(fd);
	int track = (cur_wp >= 0) ? rt->active_script_ord : 0;
	dom_serialize_node(doc, &b, 1, track, &cur_ord, &script_end);

	unsigned char *nsrc = (unsigned char *)mem_alloc(b.len + 1);
	if (nsrc) {
		if (b.len && b.data) memcpy(nsrc, b.data, b.len);
		nsrc[b.len] = '\0';
		if (fd->js->src) mem_free(fd->js->src);
		fd->js->src = nsrc;
		fd->js->srclen = (int)b.len;
		fd->js->wrote = 1;
		if (cur_wp >= -1) {
			int new_wp = -2;
			if (cur_wp >= 0 && script_end > 0 && script_end <= b.len) {
				new_wp = (int)(b.len - script_end);
				fd->script_t = (int)script_end;
			}
			jsint_set_active_wrote(fd, new_wp);
		}
	}
	db_free(&b);
	rt->dom_dirty = 0;

	if (flush_now && cur_wp < 0 && fd->script_t == -1 && fd->f_data) {
		fd->done = 0;
		fd->parsed_done = 0;
		fd_loaded(NULL, fd);
		fd->js->wrote = 0;
	}
}

/* =========================================================================
 * CSS Selector Matching
 * ========================================================================= */

static int class_attr_contains(const char *cls_attr, const char *want) {
	if (!cls_attr || !want || !*want) return 0;
	size_t wlen = strlen(want);
	const char *p = cls_attr;
	while (*p) {
		while (*p && isspace((unsigned char)*p)) p++;
		if (!*p) break;
		const char *s = p;
		while (*p && !isspace((unsigned char)*p)) p++;
		if ((size_t)(p - s) == wlen && memcmp(s, want, wlen) == 0) return 1;
	}
	return 0;
}

static int match_simple_selector(const dom_node_t *n, const char *sel, size_t len) {
	if (!n || n->kind != DOM_ELEMENT) return 0;
	const char *p = sel, *end = sel + len;
	while (p < end && isspace((unsigned char)*p)) p++;
	while (end > p && isspace((unsigned char)end[-1])) end--;
	if (p >= end) return 0;
	if (end - p == 1 && *p == '*') return 1;

	/* Optional tag prefix */
	if (*p != '#' && *p != '.' && *p != '[' && *p != ':') {
		const char *ts = p;
		while (p < end && *p != '#' && *p != '.' && *p != '[' && *p != ':') p++;
		if (!(p - ts == 1 && *ts == '*')) {
			if (!n->tag || strlen(n->tag) != (size_t)(p - ts) || strncasecmp(n->tag, ts, (size_t)(p - ts)) != 0)
				return 0;
		}
	}
	while (p < end) {
		if (*p == '#') {
			p++;
			const char *is = p;
			while (p < end && *p != '#' && *p != '.' && *p != '[' && *p != ':') p++;
			const char *id = dom_get_attr(n, "id");
			if (!id || strlen(id) != (size_t)(p - is) || memcmp(id, is, (size_t)(p - is)) != 0) return 0;
		} else if (*p == '.') {
			p++;
			const char *cs = p;
			while (p < end && *p != '#' && *p != '.' && *p != '[' && *p != ':') p++;
			char *cname = (char *)malloc((size_t)(p - cs) + 1);
			memcpy(cname, cs, (size_t)(p - cs)); cname[p - cs] = '\0';
			int ok = class_attr_contains(dom_get_attr(n, "class"), cname);
			free(cname);
			if (!ok) return 0;
		} else if (*p == '[') {
			p++;
			const char *be = (const char *)memchr(p, ']', (size_t)(end - p));
			if (!be) return 0;
			const char *as = p;
			while (p < be && *p != '=' && *p != '~' && *p != '^' && *p != '$' && *p != '*' && !isspace((unsigned char)*p)) p++;
			char *an = xdupn_lower(as, (size_t)(p - as));
			const char *av = dom_get_attr(n, an);
			free(an);
			while (p < be && isspace((unsigned char)*p)) p++;
			if (p >= be) {
				if (!av) return 0;
			} else {
				char op = '=';
				if (*p != '=') op = *p++;
				if (p < be && *p == '=') p++;
				while (p < be && isspace((unsigned char)*p)) p++;
				const char *vs = p, *ve = be;
				while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
				if (ve - vs >= 2 && ((vs[0] == '"' && ve[-1] == '"') || (vs[0] == '\'' && ve[-1] == '\''))) {
					vs++; ve--;
				}
				if (!av) return 0;
				size_t vl = (size_t)(ve - vs), al = strlen(av);
				int ok = 0;
				if (op == '=') ok = (al == vl && memcmp(av, vs, vl) == 0);
				else if (op == '^') ok = (al >= vl && memcmp(av, vs, vl) == 0);
				else if (op == '$') ok = (al >= vl && memcmp(av + al - vl, vs, vl) == 0);
				else if (op == '*') {
					char *sub = (char *)malloc(vl + 1);
					memcpy(sub, vs, vl); sub[vl] = '\0';
					ok = (strstr(av, sub) != NULL);
					free(sub);
				}
				if (!ok) return 0;
			}
			p = be + 1;
		} else if (*p == ':') {
			/* ignore simple pseudo-classes gracefully */
			p++;
			while (p < end && *p != '#' && *p != '.' && *p != '[') p++;
		} else {
			return 0;
		}
	}
	return 1;
}

static int match_complex_selector(const dom_node_t *n, const char *sel, size_t len) {
	/* Right-to-left combinator matcher (supports space and '>') */
	const char *parts[16];
	size_t plens[16];
	char combs[16];
	int np = 0;

	const char *p = sel, *end = sel + len;
	while (p < end && np < 16) {
		while (p < end && isspace((unsigned char)*p)) p++;
		if (p >= end) break;
		char comb = ' ';
		if (*p == '>') { comb = '>'; p++; while (p < end && isspace((unsigned char)*p)) p++; }
		const char *s = p;
		int in_br = 0;
		while (p < end) {
			if (*p == '[') in_br = 1;
			else if (*p == ']') in_br = 0;
			else if (!in_br && (isspace((unsigned char)*p) || *p == '>')) break;
			p++;
		}
		if (p > s) {
			parts[np] = s; plens[np] = (size_t)(p - s); combs[np] = comb; np++;
		}
	}
	if (np == 0) return 0;
	if (!match_simple_selector(n, parts[np - 1], plens[np - 1])) return 0;
	const dom_node_t *cur = n;
	for (int i = np - 2; i >= 0; i--) {
		char comb = combs[i + 1];
		if (comb == '>') {
			cur = cur->parent;
			if (!cur || !match_simple_selector(cur, parts[i], plens[i])) return 0;
		} else {
			cur = cur->parent;
			while (cur && !match_simple_selector(cur, parts[i], plens[i])) cur = cur->parent;
			if (!cur) return 0;
		}
	}
	return 1;
}

static int dom_matches_selector(const dom_node_t *n, const char *sel_list) {
	if (!n || n->kind != DOM_ELEMENT || !sel_list) return 0;
	const char *p = sel_list;
	while (*p) {
		const char *c = strchr(p, ',');
		size_t l = c ? (size_t)(c - p) : strlen(p);
		if (match_complex_selector(n, p, l)) return 1;
		if (!c) break;
		p = c + 1;
	}
	return 0;
}

/* =========================================================================
 * CSS Inline Style Helper (camelCase <-> kebab-case)
 * ========================================================================= */

static void camel_to_kebab(const char *s, char *out, size_t cap) {
	size_t j = 0;
	for (size_t i = 0; s[i] && j + 2 < cap; i++) {
		unsigned char c = (unsigned char)s[i];
		if (isupper(c) && j > 0) {
			out[j++] = '-';
			out[j++] = (char)tolower(c);
		} else {
			out[j++] = (char)tolower(c);
		}
	}
	out[j] = '\0';
}

static int style_get_decl(const char *style, const char *prop_kebab, char *out, size_t cap) {
	if (out && cap) out[0] = '\0';
	if (!style || !prop_kebab || !*prop_kebab) return 0;
	const char *p = style;
	int found = 0;
	while (*p) {
		while (*p && (isspace((unsigned char)*p) || *p == ';')) p++;
		if (!*p) break;
		const char *ks = p;
		while (*p && *p != ':' && *p != ';') p++;
		const char *ke = p;
		while (ke > ks && isspace((unsigned char)ke[-1])) ke--;
		if (*p != ':') { while (*p && *p != ';') p++; continue; }
		p++;
		while (*p && isspace((unsigned char)*p)) p++;
		const char *vs = p;
		while (*p && *p != ';') p++;
		const char *ve = p;
		while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
		if (strlen(prop_kebab) == (size_t)(ke - ks) &&
		    strncasecmp(ks, prop_kebab, (size_t)(ke - ks)) == 0) {
			if (out && cap) {
				size_t vl = (size_t)(ve - vs);
				if (vl >= cap) vl = cap - 1;
				memcpy(out, vs, vl); out[vl] = '\0';
			}
			found = 1;
		}
	}
	return found;
}

static void style_set_decl(dom_node_t *n, const char *prop_kebab, const char *val) {
	const char *cur = dom_get_attr(n, "style");
	dbuf_t b; db_init(&b);
	if (cur) {
		const char *p = cur;
		while (*p) {
			while (*p && (isspace((unsigned char)*p) || *p == ';')) p++;
			if (!*p) break;
			const char *ks = p;
			while (*p && *p != ':' && *p != ';') p++;
			const char *ke = p;
			while (ke > ks && isspace((unsigned char)ke[-1])) ke--;
			if (*p != ':') { while (*p && *p != ';') p++; continue; }
			p++;
			while (*p && isspace((unsigned char)*p)) p++;
			const char *vs = p;
			while (*p && *p != ';') p++;
			const char *ve = p;
			while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
			if (!(strlen(prop_kebab) == (size_t)(ke - ks) &&
			      strncasecmp(ks, prop_kebab, (size_t)(ke - ks)) == 0)) {
				if (b.len > 0) db_adds(&b, "; ");
				db_add(&b, ks, (size_t)(ke - ks));
				db_adds(&b, ": ");
				db_add(&b, vs, (size_t)(ve - vs));
			}
		}
	}
	if (val && *val) {
		if (b.len > 0) db_adds(&b, "; ");
		db_adds(&b, prop_kebab);
		db_adds(&b, ": ");
		db_adds(&b, val);
	}
	if (b.len > 0) dom_set_attr(n, "style", b.data);
	else dom_remove_attr(n, "style");
	db_free(&b);
}

/* =========================================================================
 * JS DOM Wrapper Objects & Native Methods
 * ========================================================================= */

static ipo_js_val_t wrap_dom_node(ipo_js_runtime_t *rt, dom_node_t *n);
static ipo_js_obj_t *wrap_style(ipo_js_runtime_t *rt, dom_node_t *n);
static ipo_js_obj_t *wrap_classlist(ipo_js_runtime_t *rt, dom_node_t *n);
static ipo_js_obj_t *wrap_dataset(ipo_js_runtime_t *rt, dom_node_t *n);

static void collect_text_rec(const dom_node_t *root, dbuf_t *b) {
	for (dom_node_t *n = (dom_node_t *)root; n; n = dom_next_dfs(root, n)) {
		if (n->kind == DOM_TEXT) db_adds(b, n->text ? n->text : "");
	}
}

static void add_listener_node(dom_listener_t **head, const char *type, ipo_js_val_t fn) {
	if (!head || !type || !ipo_js_is_callable(fn)) return;
	dom_listener_t *nl = (dom_listener_t *)calloc(1, sizeof(dom_listener_t));
	nl->type = xdupn_lower(type, strlen(type));
	nl->fn = fn;
	if (fn.type == JS_T_OBJECT && fn.u.obj) ipo_js_gc_pin(fn.u.obj);
	nl->next = NULL;
	if (!*head) { *head = nl; return; }
	dom_listener_t *tail = *head;
	while (tail->next) tail = tail->next;
	tail->next = nl;
}

static void set_on_prop_listener(ipo_js_runtime_t *rt, dom_node_t *n, const char *evtype, ipo_js_val_t fn) {
	/* Remove prior JS-property listener? Keep addEventListener intact; store or replace */
	if (ipo_js_is_callable(fn)) {
		/* Replace first matching listener or append */
		for (dom_listener_t *l = n->listeners; l; l = l->next) {
			if (strcmp(l->type, evtype) == 0) {
				if (l->fn.type == JS_T_OBJECT && l->fn.u.obj) ipo_js_gc_unpin(l->fn.u.obj);
				l->fn = fn;
				ipo_js_gc_pin(fn.u.obj);
				rt->dom_dirty = 1;
				return;
			}
		}
		add_listener_node(&n->listeners, evtype, fn);
		rt->dom_dirty = 1;
	}
}

static void dom_query_collect(ipo_js_runtime_t *rt, dom_node_t *root, const char *sel,
                              ipo_js_obj_t *arr, int single) {
	for (dom_node_t *c = dom_next_dfs(root, root); c; c = dom_next_dfs(root, c)) {
		if (c->kind == DOM_ELEMENT && dom_matches_selector(c, sel)) {
			ipo_js_arr_push(rt->vm, arr, wrap_dom_node(rt, c));
			if (single) return;
		}
	}
}

static void dom_tag_collect(ipo_js_runtime_t *rt, dom_node_t *root, const char *tag, ipo_js_obj_t *arr) {
	for (dom_node_t *c = dom_next_dfs(root, root); c; c = dom_next_dfs(root, c)) {
		if (c->kind == DOM_ELEMENT) {
			if (strcmp(tag, "*") == 0 || (c->tag && ci_eq(c->tag, tag)))
				ipo_js_arr_push(rt->vm, arr, wrap_dom_node(rt, c));
		}
	}
}

static void dom_class_collect(ipo_js_runtime_t *rt, dom_node_t *root, const char *cls, ipo_js_obj_t *arr) {
	for (dom_node_t *c = dom_next_dfs(root, root); c; c = dom_next_dfs(root, c)) {
		if (c->kind == DOM_ELEMENT && class_attr_contains(dom_get_attr(c, "class"), cls)) {
			ipo_js_arr_push(rt->vm, arr, wrap_dom_node(rt, c));
		}
	}
}

static int el_host_get(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t *out) {
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *n = (dom_node_t *)obj->host_ptr;
	if (!n) return 0;

	if (strcmp(key, "nodeType") == 0) {
		*out = ipo_js_num(n->kind == DOM_ELEMENT ? 1 : (n->kind == DOM_TEXT ? 3 : 8));
		return 1;
	}
	if (strcmp(key, "tagName") == 0 || strcmp(key, "nodeName") == 0) {
		if (n->kind == DOM_ELEMENT && n->tag) {
			char up[64];
			size_t i = 0;
			for (; n->tag[i] && i + 1 < sizeof(up); i++) up[i] = (char)toupper((unsigned char)n->tag[i]);
			up[i] = '\0';
			*out = ipo_js_str_cstr(vm, up);
		} else {
			*out = ipo_js_str_cstr(vm, n->kind == DOM_TEXT ? "#text" : "#comment");
		}
		return 1;
	}
	if (strcmp(key, "nodeValue") == 0 || strcmp(key, "data") == 0) {
		*out = (n->kind == DOM_TEXT || n->kind == DOM_COMMENT) ? ipo_js_str_cstr(vm, n->text ? n->text : "") : ipo_js_null();
		return 1;
	}
	if (strcmp(key, "textContent") == 0 || strcmp(key, "innerText") == 0) {
		dbuf_t b; db_init(&b);
		collect_text_rec(n, &b);
		*out = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
		db_free(&b);
		return 1;
	}
	if (strcmp(key, "innerHTML") == 0 || strcmp(key, "outerHTML") == 0) {
		dbuf_t b; db_init(&b);
		dom_serialize_node(n, &b, strcmp(key, "innerHTML") == 0 ? 1 : 0, 0, NULL, NULL);
		*out = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
		db_free(&b);
		return 1;
	}
	if (strcmp(key, "id") == 0) {
		const char *v = dom_get_attr(n, "id");
		*out = ipo_js_str_cstr(vm, v ? v : "");
		return 1;
	}
	if (strcmp(key, "className") == 0) {
		const char *v = dom_get_attr(n, "class");
		*out = ipo_js_str_cstr(vm, v ? v : "");
		return 1;
	}
	if (strcmp(key, "style") == 0) {
		*out = ipo_js_obj_val(wrap_style(rt, n));
		return 1;
	}
	if (strcmp(key, "classList") == 0) {
		*out = ipo_js_obj_val(wrap_classlist(rt, n));
		return 1;
	}
	if (strcmp(key, "dataset") == 0) {
		*out = ipo_js_obj_val(wrap_dataset(rt, n));
		return 1;
	}
	if (strcmp(key, "value") == 0) {
		if (!n->attr_dirty_form) {
			long jid = 0;
			struct form_control *fc = match_form_control(rt, n, &jid);
			if (fc && (fc->type == FC_TEXT || fc->type == FC_PASSWORD || fc->type == FC_TEXTAREA)) {
				unsigned char *v = js_upcall_get_form_element_value((struct f_data_c *)rt->ctx->ptr, rt->ctx->js_id, jid);
				if (v) {
					*out = ipo_js_str_cstr(vm, (const char *)v);
					mem_free(v);
					return 1;
				}
			}
			if (fc && fc->type == FC_SELECT) {
				struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
				struct form_state *fs = find_form_state(fd, fc);
				if (fs && fs->state >= 0 && fs->state < fc->nvalues && fc->values[fs->state]) {
					*out = ipo_js_str_cstr(vm, (const char *)fc->values[fs->state]);
					return 1;
				}
			}
		}
		if (n->tag && strcmp(n->tag, "textarea") == 0) {
			dbuf_t b; db_init(&b); collect_text_rec(n, &b);
			*out = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
			db_free(&b);
			return 1;
		}
		const char *v = dom_get_attr(n, "value");
		*out = ipo_js_str_cstr(vm, v ? v : "");
		return 1;
	}
	if (strcmp(key, "checked") == 0) {
		if (!n->attr_dirty_form) {
			long jid = 0;
			struct form_control *fc = match_form_control(rt, n, &jid);
			if (fc && (fc->type == FC_CHECKBOX || fc->type == FC_RADIO)) {
				int chk = js_upcall_get_checkbox_radio_checked((struct f_data_c *)rt->ctx->ptr, rt->ctx->js_id, jid);
				*out = ipo_js_bool(chk != 0);
				return 1;
			}
		}
		*out = ipo_js_bool(dom_get_attr(n, "checked") != NULL);
		return 1;
	}
	if (strcmp(key, "disabled") == 0 || strcmp(key, "hidden") == 0) {
		*out = ipo_js_bool(dom_get_attr(n, key) != NULL);
		return 1;
	}
	if (strcmp(key, "href") == 0 || strcmp(key, "src") == 0 || strcmp(key, "name") == 0 ||
	    strcmp(key, "type") == 0 || strcmp(key, "action") == 0 || strcmp(key, "method") == 0 ||
	    strcmp(key, "placeholder") == 0 || strcmp(key, "title") == 0 || strcmp(key, "alt") == 0) {
		const char *v = dom_get_attr(n, key);
		*out = ipo_js_str_cstr(vm, v ? v : "");
		return 1;
	}
	if (strcmp(key, "parentNode") == 0 || strcmp(key, "parentElement") == 0) {
		if (!n->parent || (strcmp(key, "parentElement") == 0 && n->parent->kind != DOM_ELEMENT))
			*out = (strcmp(key, "parentNode") == 0 && n->parent == rt->doc)
			           ? ipo_js_obj_val(rt->doc_obj) : ipo_js_null();
		else *out = wrap_dom_node(rt, n->parent);
		return 1;
	}
	if (strcmp(key, "firstChild") == 0) { *out = wrap_dom_node(rt, n->first_child); return 1; }
	if (strcmp(key, "lastChild") == 0)  { *out = wrap_dom_node(rt, n->last_child);  return 1; }
	if (strcmp(key, "nextSibling") == 0)     { *out = wrap_dom_node(rt, n->next_sibling); return 1; }
	if (strcmp(key, "previousSibling") == 0) { *out = wrap_dom_node(rt, n->prev_sibling); return 1; }
	if (strcmp(key, "firstElementChild") == 0) {
		dom_node_t *c = n->first_child;
		while (c && c->kind != DOM_ELEMENT) c = c->next_sibling;
		*out = wrap_dom_node(rt, c);
		return 1;
	}
	if (strcmp(key, "lastElementChild") == 0) {
		dom_node_t *c = n->last_child;
		while (c && c->kind != DOM_ELEMENT) c = c->prev_sibling;
		*out = wrap_dom_node(rt, c);
		return 1;
	}
	if (strcmp(key, "nextElementSibling") == 0) {
		dom_node_t *c = n->next_sibling;
		while (c && c->kind != DOM_ELEMENT) c = c->next_sibling;
		*out = wrap_dom_node(rt, c);
		return 1;
	}
	if (strcmp(key, "previousElementSibling") == 0) {
		dom_node_t *c = n->prev_sibling;
		while (c && c->kind != DOM_ELEMENT) c = c->prev_sibling;
		*out = wrap_dom_node(rt, c);
		return 1;
	}
	if (strcmp(key, "children") == 0 || strcmp(key, "childNodes") == 0) {
		int only_el = (strcmp(key, "children") == 0);
		ipo_js_obj_t *arr = ipo_js_new_array(vm, 0);
		size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(arr));
		for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
			if (!only_el || c->kind == DOM_ELEMENT) ipo_js_arr_push(vm, arr, wrap_dom_node(rt, c));
		}
		ipo_js_gc_pop_roots(vm, top);
		*out = ipo_js_obj_val(arr);
		return 1;
	}
	if (strcmp(key, "offsetWidth") == 0 || strcmp(key, "clientWidth") == 0) {
		*out = ipo_js_num(600.0); return 1;
	}
	if (strcmp(key, "offsetHeight") == 0 || strcmp(key, "clientHeight") == 0) {
		*out = ipo_js_num(32.0); return 1;
	}
	return 0;
}

static int el_host_set(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t val) {
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *n = (dom_node_t *)obj->host_ptr;
	if (!n) return 0;

	if (strcmp(key, "textContent") == 0 || strcmp(key, "innerText") == 0) {
		while (n->first_child) {
			dom_node_t *c = n->first_child;
			dom_detach(c);
			dom_free_subtree(rt, c);
		}
		const char *s = (val.type == JS_T_NULL || val.type == JS_T_UNDEFINED) ? "" : ipo_js_cstr(vm, val, NULL);
		if (*s) {
			dom_node_t *tn = dom_alloc_node(rt, DOM_TEXT);
			tn->text = xdup(s);
			dom_append_child(n, tn);
		}
		rt->dom_dirty = 1;
		return 1;
	}
	if (strcmp(key, "innerHTML") == 0) {
		while (n->first_child) {
			dom_node_t *c = n->first_child;
			dom_detach(c);
			dom_free_subtree(rt, c);
		}
		size_t slen = 0;
		const char *s = (val.type == JS_T_NULL || val.type == JS_T_UNDEFINED) ? "" : ipo_js_cstr(vm, val, &slen);
		if (slen > 0) parse_html_into(rt, n, s, slen);
		rt->dom_dirty = 1;
		return 1;
	}
	if (strcmp(key, "nodeValue") == 0 || strcmp(key, "data") == 0) {
		if (n->kind == DOM_TEXT || n->kind == DOM_COMMENT) {
			free(n->text);
			n->text = xdup(ipo_js_cstr(vm, val, NULL));
			rt->dom_dirty = 1;
			return 1;
		}
	}
	if (strcmp(key, "id") == 0)        { dom_set_attr(n, "id", ipo_js_cstr(vm, val, NULL)); rt->dom_dirty = 1; return 1; }
	if (strcmp(key, "className") == 0) { dom_set_attr(n, "class", ipo_js_cstr(vm, val, NULL)); rt->dom_dirty = 1; return 1; }
	if (strcmp(key, "value") == 0) {
		const char *vs = (val.type == JS_T_NULL || val.type == JS_T_UNDEFINED) ? "" : ipo_js_cstr(vm, val, NULL);
		n->attr_dirty_form = 1;
		if (n->tag && strcmp(n->tag, "textarea") == 0) {
			while (n->first_child) { dom_node_t *c = n->first_child; dom_detach(c); dom_free_subtree(rt, c); }
			if (*vs) { dom_node_t *tn = dom_alloc_node(rt, DOM_TEXT); tn->text = xdup(vs); dom_append_child(n, tn); }
		} else {
			dom_set_attr(n, "value", vs);
		}
		long jid = 0;
		struct form_control *fc = match_form_control(rt, n, &jid);
		if (fc && (fc->type == FC_TEXT || fc->type == FC_PASSWORD || fc->type == FC_TEXTAREA)) {
			js_upcall_set_form_element_value((struct f_data_c *)rt->ctx->ptr, rt->ctx->js_id, jid, stracpy((unsigned char *)vs));
		}
		rt->dom_dirty = 1;
		return 1;
	}
	if (strcmp(key, "checked") == 0) {
		int b = ipo_js_to_bool(val);
		n->attr_dirty_form = 1;
		if (b) dom_set_attr(n, "checked", "checked");
		else dom_remove_attr(n, "checked");
		long jid = 0;
		struct form_control *fc = match_form_control(rt, n, &jid);
		if (fc && (fc->type == FC_CHECKBOX || fc->type == FC_RADIO)) {
			js_upcall_set_checkbox_radio_checked((struct f_data_c *)rt->ctx->ptr, rt->ctx->js_id, jid, b);
		}
		rt->dom_dirty = 1;
		return 1;
	}
	if (strcmp(key, "disabled") == 0 || strcmp(key, "hidden") == 0) {
		if (ipo_js_to_bool(val)) dom_set_attr(n, key, key);
		else dom_remove_attr(n, key);
		rt->dom_dirty = 1;
		return 1;
	}
	if (strcmp(key, "href") == 0 || strcmp(key, "src") == 0 || strcmp(key, "name") == 0 ||
	    strcmp(key, "type") == 0 || strcmp(key, "action") == 0 || strcmp(key, "method") == 0 ||
	    strcmp(key, "placeholder") == 0 || strcmp(key, "title") == 0 || strcmp(key, "alt") == 0) {
		dom_set_attr(n, key, ipo_js_cstr(vm, val, NULL));
		rt->dom_dirty = 1;
		return 1;
	}
	if (strncmp(key, "on", 2) == 0 && key[2] != '\0') {
		set_on_prop_listener(rt, n, key + 2, val);
		return 0; /* also allow reading back `el.onclick` from own prop */
	}
	return 0;
}

/* Style object hooks */
static int style_host_get(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t *out) {
	dom_node_t *n = (dom_node_t *)obj->host_ptr;
	if (!n) return 0;
	if (strcmp(key, "cssText") == 0) {
		const char *s = dom_get_attr(n, "style");
		*out = ipo_js_str_cstr(vm, s ? s : "");
		return 1;
	}
	if (strcmp(key, "setProperty") == 0 || strcmp(key, "getPropertyValue") == 0 || strcmp(key, "removeProperty") == 0)
		return 0;
	char keb[96], val[256];
	camel_to_kebab(key, keb, sizeof(keb));
	style_get_decl(dom_get_attr(n, "style"), keb, val, sizeof(val));
	*out = ipo_js_str_cstr(vm, val);
	return 1;
}

static int style_host_set(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t val) {
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *n = (dom_node_t *)obj->host_ptr;
	if (!n) return 0;
	if (strcmp(key, "cssText") == 0) {
		const char *s = ipo_js_cstr(vm, val, NULL);
		if (*s) dom_set_attr(n, "style", s);
		else dom_remove_attr(n, "style");
		rt->dom_dirty = 1;
		return 1;
	}
	char keb[96];
	camel_to_kebab(key, keb, sizeof(keb));
	style_set_decl(n, keb, (val.type == JS_T_NULL || val.type == JS_T_UNDEFINED) ? "" : ipo_js_cstr(vm, val, NULL));
	rt->dom_dirty = 1;
	return 1;
}

static ipo_js_val_t js_style_method(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	int mode = (int)(intptr_t)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || !tv.u.obj->host_ptr) return ipo_js_undef();
	dom_node_t *n = (dom_node_t *)tv.u.obj->host_ptr;
	const char *prop = (argc >= 1) ? ipo_js_cstr(vm, argv[0], NULL) : "";
	if (mode == 0) {
		char val[256];
		style_get_decl(dom_get_attr(n, "style"), prop, val, sizeof(val));
		return ipo_js_str_cstr(vm, val);
	}
	if (mode == 1) {
		const char *v = (argc >= 2) ? ipo_js_cstr(vm, argv[1], NULL) : "";
		style_set_decl(n, prop, v);
		rt->dom_dirty = 1;
		return ipo_js_undef();
	}
	style_set_decl(n, prop, "");
	rt->dom_dirty = 1;
	return ipo_js_undef();
}

static ipo_js_obj_t *wrap_style(ipo_js_runtime_t *rt, dom_node_t *n) {
	if (n->style_wrapper) return n->style_wrapper;
	ipo_js_obj_t *o = ipo_js_new_object(rt->vm, NULL);
	o->host_tag = HTAG_STYLE;
	o->host_ptr = n;
	o->host_get = style_host_get;
	o->host_set = style_host_set;
	ipo_js_set_prop(rt->vm, o, "getPropertyValue", ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "getPropertyValue", js_style_method, (void *)0, 0)));
	ipo_js_set_prop(rt->vm, o, "setProperty",      ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "setProperty",      js_style_method, (void *)1, 0)));
	ipo_js_set_prop(rt->vm, o, "removeProperty",   ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "removeProperty",   js_style_method, (void *)2, 0)));
	n->style_wrapper = o;
	return o;
}

/* classList helper */
static void classlist_modify(dom_node_t *n, const char *cls, int op /* 0=add, 1=remove */) {
	const char *cur = dom_get_attr(n, "class");
	dbuf_t b; db_init(&b);
	int seen = 0;
	if (cur) {
		const char *p = cur;
		while (*p) {
			while (*p && isspace((unsigned char)*p)) p++;
			if (!*p) break;
			const char *s = p;
			while (*p && !isspace((unsigned char)*p)) p++;
			size_t l = (size_t)(p - s);
			if (l == strlen(cls) && memcmp(s, cls, l) == 0) {
				seen = 1;
				if (op == 1) continue;
			}
			if (b.len > 0) db_addc(&b, ' ');
			db_add(&b, s, l);
		}
	}
	if (op == 0 && !seen && *cls) {
		if (b.len > 0) db_addc(&b, ' ');
		db_adds(&b, cls);
	}
	dom_set_attr(n, "class", b.data ? b.data : "");
	db_free(&b);
}

static ipo_js_val_t js_classlist_method(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	int op = (int)(intptr_t)ud; /* 0=add, 1=remove, 2=toggle, 3=contains, 4=replace */
	if (tv.type != JS_T_OBJECT || !tv.u.obj || !tv.u.obj->host_ptr) return ipo_js_undef();
	dom_node_t *n = (dom_node_t *)tv.u.obj->host_ptr;
	if (argc < 1) return ipo_js_bool(0);
	const char *c0 = ipo_js_cstr(vm, argv[0], NULL);
	if (op == 3) return ipo_js_bool(class_attr_contains(dom_get_attr(n, "class"), c0));
	if (op == 0) {
		for (int i = 0; i < argc; i++) classlist_modify(n, ipo_js_cstr(vm, argv[i], NULL), 0);
		rt->dom_dirty = 1;
		return ipo_js_undef();
	}
	if (op == 1) {
		for (int i = 0; i < argc; i++) classlist_modify(n, ipo_js_cstr(vm, argv[i], NULL), 1);
		rt->dom_dirty = 1;
		return ipo_js_undef();
	}
	if (op == 2) {
		int has = class_attr_contains(dom_get_attr(n, "class"), c0);
		int force = (argc >= 2) ? ipo_js_to_bool(argv[1]) : !has;
		classlist_modify(n, c0, force ? 0 : 1);
		rt->dom_dirty = 1;
		return ipo_js_bool(force);
	}
	if (op == 4 && argc >= 2) {
		const char *c1 = ipo_js_cstr(vm, argv[1], NULL);
		if (class_attr_contains(dom_get_attr(n, "class"), c0)) {
			classlist_modify(n, c0, 1);
			classlist_modify(n, c1, 0);
			rt->dom_dirty = 1;
			return ipo_js_bool(1);
		}
		return ipo_js_bool(0);
	}
	return ipo_js_undef();
}

static ipo_js_obj_t *wrap_classlist(ipo_js_runtime_t *rt, dom_node_t *n) {
	if (n->classlist_wrapper) return n->classlist_wrapper;
	ipo_js_obj_t *o = ipo_js_new_object(rt->vm, NULL);
	o->host_tag = HTAG_CLASSLIST;
	o->host_ptr = n;
	ipo_js_set_prop(rt->vm, o, "add",      ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "add",      js_classlist_method, (void *)0, 0)));
	ipo_js_set_prop(rt->vm, o, "remove",   ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "remove",   js_classlist_method, (void *)1, 0)));
	ipo_js_set_prop(rt->vm, o, "toggle",   ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "toggle",   js_classlist_method, (void *)2, 0)));
	ipo_js_set_prop(rt->vm, o, "contains", ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "contains", js_classlist_method, (void *)3, 0)));
	ipo_js_set_prop(rt->vm, o, "replace",  ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "replace",  js_classlist_method, (void *)4, 0)));
	n->classlist_wrapper = o;
	return o;
}

static int dataset_host_get(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t *out) {
	dom_node_t *n = (dom_node_t *)obj->host_ptr;
	if (!n) return 0;
	char keb[96], full[112];
	camel_to_kebab(key, keb, sizeof(keb));
	snprintf(full, sizeof(full), "data-%s", keb);
	const char *v = dom_get_attr(n, full);
	if (!v) return 0;
	*out = ipo_js_str_cstr(vm, v);
	return 1;
}

static int dataset_host_set(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t val) {
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *n = (dom_node_t *)obj->host_ptr;
	if (!n) return 0;
	char keb[96], full[112];
	camel_to_kebab(key, keb, sizeof(keb));
	snprintf(full, sizeof(full), "data-%s", keb);
	dom_set_attr(n, full, ipo_js_cstr(vm, val, NULL));
	rt->dom_dirty = 1;
	return 1;
}

static ipo_js_obj_t *wrap_dataset(ipo_js_runtime_t *rt, dom_node_t *n) {
	if (n->dataset_wrapper) return n->dataset_wrapper;
	ipo_js_obj_t *o = ipo_js_new_object(rt->vm, NULL);
	o->host_tag = HTAG_DATASET;
	o->host_ptr = n;
	o->host_get = dataset_host_get;
	o->host_set = dataset_host_set;
	n->dataset_wrapper = o;
	return o;
}

/* Element Methods */
static dom_node_t *unwrap_dom_node(ipo_js_val_t v) {
	if (v.type != JS_T_OBJECT || !v.u.obj || v.u.obj->host_tag != HTAG_ELEMENT) return NULL;
	return (dom_node_t *)v.u.obj->host_ptr;
}

static ipo_js_val_t js_el_get_attr(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	dom_node_t *n = unwrap_dom_node(tv);
	if (!n || argc < 1) return ipo_js_null();
	const char *v = dom_get_attr(n, ipo_js_cstr(vm, argv[0], NULL));
	return v ? ipo_js_str_cstr(vm, v) : ipo_js_null();
}

static ipo_js_val_t js_el_set_attr(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *n = unwrap_dom_node(tv);
	if (!n || argc < 2) return ipo_js_undef();
	const char *an = ipo_js_cstr(vm, argv[0], NULL);
	const char *av = ipo_js_cstr(vm, argv[1], NULL);
	dom_set_attr(n, an, av);
	rt->dom_dirty = 1;
	return ipo_js_undef();
}

static ipo_js_val_t js_el_has_remove_attr(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int is_rem = (int)(intptr_t)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *n = unwrap_dom_node(tv);
	if (!n || argc < 1) return is_rem ? ipo_js_undef() : ipo_js_bool(0);
	const char *an = ipo_js_cstr(vm, argv[0], NULL);
	if (!is_rem) return ipo_js_bool(dom_get_attr(n, an) != NULL);
	dom_remove_attr(n, an);
	rt->dom_dirty = 1;
	return ipo_js_undef();
}

static ipo_js_val_t js_el_append_insert(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int mode = (int)(intptr_t)ud; /* 0=appendChild, 1=insertBefore, 2=append, 3=prepend */
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *p = unwrap_dom_node(tv);
	if (!p || argc < 1) return ipo_js_undef();
	if (mode == 0) {
		dom_node_t *c = unwrap_dom_node(argv[0]);
		if (c) { dom_append_child(p, c); rt->dom_dirty = 1; }
		return argv[0];
	}
	if (mode == 1) {
		dom_node_t *c = unwrap_dom_node(argv[0]);
		dom_node_t *ref = (argc >= 2) ? unwrap_dom_node(argv[1]) : NULL;
		if (c) { dom_insert_before(p, c, ref); rt->dom_dirty = 1; }
		return argv[0];
	}
	dom_node_t *anchor = (mode == 3) ? p->first_child : NULL;
	for (int i = 0; i < argc; i++) {
		dom_node_t *c = unwrap_dom_node(argv[i]);
		if (!c) {
			c = dom_alloc_node(rt, DOM_TEXT);
			c->text = xdup(ipo_js_cstr(vm, argv[i], NULL));
		}
		if (anchor) dom_insert_before(p, c, anchor);
		else dom_append_child(p, c);
	}
	rt->dom_dirty = 1;
	return ipo_js_undef();
}

static ipo_js_val_t js_el_remove(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int is_child = (int)(intptr_t)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *n = unwrap_dom_node(tv);
	if (!n) return ipo_js_undef();
	if (!is_child) { dom_detach(n); rt->dom_dirty = 1; return ipo_js_undef(); }
	if (argc >= 1) {
		dom_node_t *c = unwrap_dom_node(argv[0]);
		if (c && c->parent == n) { dom_detach(c); rt->dom_dirty = 1; return argv[0]; }
	}
	return ipo_js_undef();
}

static dom_node_t *dom_clone_rec(ipo_js_runtime_t *rt, const dom_node_t *src, int deep) {
	dom_node_t *c = dom_alloc_node(rt, src->kind);
	if (src->tag) c->tag = xdup(src->tag);
	if (src->text) c->text = xdup(src->text);
	for (size_t i = 0; i < src->attr_len; i++) dom_set_attr(c, src->attrs[i].name, src->attrs[i].value);
	if (deep) {
		for (dom_node_t *ch = src->first_child; ch; ch = ch->next_sibling) {
			dom_append_child(c, dom_clone_rec(rt, ch, 1));
		}
	}
	return c;
}

static ipo_js_val_t js_el_clone(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *n = unwrap_dom_node(tv);
	if (!n) return ipo_js_null();
	int deep = (argc >= 1) ? ipo_js_to_bool(argv[0]) : 0;
	return wrap_dom_node(rt, dom_clone_rec(rt, n, deep));
}

static ipo_js_val_t js_dom_query(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int mode = (int)(intptr_t)ud; /* 0=qs, 1=qsa, 2=byId, 3=byTag, 4=byClass, 5=matches, 6=closest */
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *doc = dom_ensure_parsed(rt);
	dom_node_t *root = unwrap_dom_node(tv);
	if (!root) root = doc;
	if (!root || argc < 1) return (mode == 0 || mode == 2 || mode == 6) ? ipo_js_null()
	                                                                   : ipo_js_obj_val(ipo_js_new_array(vm, 0));
	const char *q = ipo_js_cstr(vm, argv[0], NULL);
	if (mode == 2) return wrap_dom_node(rt, dom_find_by_id_rec(doc, q));
	if (mode == 5) return ipo_js_bool(dom_matches_selector(root, q));
	if (mode == 6) {
		for (dom_node_t *c = root; c && c->kind == DOM_ELEMENT; c = c->parent) {
			if (dom_matches_selector(c, q)) return wrap_dom_node(rt, c);
		}
		return ipo_js_null();
	}
	ipo_js_obj_t *arr = ipo_js_new_array(vm, 0);
	size_t top = ipo_js_gc_push_root(vm, ipo_js_obj_val(arr));
	if (mode == 0 || mode == 1) dom_query_collect(rt, root, q, arr, mode == 0);
	else if (mode == 3) dom_tag_collect(rt, root, q, arr);
	else if (mode == 4) dom_class_collect(rt, root, q, arr);
	ipo_js_gc_pop_roots(vm, top);
	if (mode == 0) return arr->u.arr.len > 0 ? arr->u.arr.items[0] : ipo_js_null();
	return ipo_js_obj_val(arr);
}

static ipo_js_val_t js_el_add_listener(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	if (argc < 2 || !ipo_js_is_callable(argv[1])) return ipo_js_undef();
	const char *type = ipo_js_cstr(vm, argv[0], NULL);
	dom_node_t *n = unwrap_dom_node(tv);
	if (n) {
		add_listener_node(&n->listeners, type, argv[1]);
		rt->dom_dirty = 1;
	} else if (tv.type == JS_T_OBJECT &&
	           (tv.u.obj == ipo_js_global(vm) || tv.u.obj == rt->doc_obj)) {
		add_listener_node(&rt->win_listeners, type, argv[1]);
		if (ci_eq(type, "DOMContentLoaded") || ci_eq(type, "load")) {
			/* Trigger cleanly once if scripts already finished */
			struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
			if (fd && fd->script_t == -1) {
				ipo_js_val_t res;
				ipo_js_call(vm, argv[1], tv, 0, NULL, &res);
			}
		}
	}
	return ipo_js_undef();
}

static ipo_js_val_t js_el_form_action(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)argc; (void)argv;
	int act = (int)(intptr_t)ud; /* 0=click, 1=focus, 2=blur, 3=submit, 4=reset */
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	dom_node_t *n = unwrap_dom_node(tv);
	if (!n) return ipo_js_undef();
	if (act == 0) {
		/* Execute JS listeners + inline onclick if present */
		ipo_js_obj_t *ev_obj = ipo_js_new_object(vm, NULL);
		ipo_js_set_prop(vm, ev_obj, "type", ipo_js_str_cstr(vm, "click"));
		ipo_js_set_prop(vm, ev_obj, "target", tv);
		ipo_js_set_prop(vm, ev_obj, "currentTarget", tv);
		ipo_js_val_t evv = ipo_js_obj_val(ev_obj);
		for (dom_listener_t *l = n->listeners; l; l = l->next) {
			if (strcmp(l->type, "click") == 0) {
				ipo_js_val_t r = ipo_js_undef();
				ipo_js_call(vm, l->fn, tv, 1, &evv, &r);
			}
		}
		const char *onc = dom_get_attr(n, "onclick");
		if (onc && *onc && strstr(onc, "__ipoDispatch(") == NULL) {
			ipo_js_eval(vm, onc, strlen(onc), "onclick", NULL);
		}
	}
	long jid = 0;
	struct form_control *fc = match_form_control(rt, n, &jid);
	if (fc && rt->ctx && rt->ctx->ptr) {
		struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
		if (act == 0) js_upcall_click(fd, rt->ctx->js_id, jid);
		else if (act == 1) js_upcall_focus(fd, rt->ctx->js_id, jid);
		else if (act == 2) js_upcall_blur(fd, rt->ctx->js_id, jid);
		else if (act == 3) js_upcall_submit(fd, rt->ctx->js_id, (fc->form_num << JS_OBJ_MASK_SIZE) | JS_OBJ_T_FORM);
		else if (act == 4) js_upcall_reset(fd, rt->ctx->js_id, (fc->form_num << JS_OBJ_MASK_SIZE) | JS_OBJ_T_FORM);
	} else if (act == 3 && n->tag && strcmp(n->tag, "form") == 0 && n->first_child) {
		/* find first control inside form to get form_num */
		for (dom_node_t *c = n->first_child; c; c = c->next_sibling) {
			long cid = 0;
			struct form_control *cfc = match_form_control(rt, c, &cid);
			if (cfc) {
				js_upcall_submit((struct f_data_c *)rt->ctx->ptr, rt->ctx->js_id,
				                 (cfc->form_num << JS_OBJ_MASK_SIZE) | JS_OBJ_T_FORM);
				break;
			}
		}
	}
	return ipo_js_undef();
}

static ipo_js_val_t wrap_dom_node(ipo_js_runtime_t *rt, dom_node_t *n) {
	if (!n) return ipo_js_null();
	if (n == rt->doc) return ipo_js_obj_val(rt->doc_obj);
	if (n->js_wrapper) return ipo_js_obj_val(n->js_wrapper);

	ipo_js_obj_t *o = ipo_js_new_object(rt->vm, NULL);
	o->host_tag = HTAG_ELEMENT;
	o->host_ptr = n;
	o->host_get = el_host_get;
	o->host_set = el_host_set;
	ipo_js_gc_pin(o);
	n->js_wrapper = o;

	ipo_js_set_prop(rt->vm, o, "getAttribute",          ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "getAttribute",          js_el_get_attr,        NULL, 0)));
	ipo_js_set_prop(rt->vm, o, "setAttribute",          ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "setAttribute",          js_el_set_attr,        NULL, 0)));
	ipo_js_set_prop(rt->vm, o, "hasAttribute",          ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "hasAttribute",          js_el_has_remove_attr, (void *)0, 0)));
	ipo_js_set_prop(rt->vm, o, "removeAttribute",       ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "removeAttribute",       js_el_has_remove_attr, (void *)1, 0)));
	ipo_js_set_prop(rt->vm, o, "appendChild",           ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "appendChild",           js_el_append_insert,   (void *)0, 0)));
	ipo_js_set_prop(rt->vm, o, "insertBefore",          ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "insertBefore",          js_el_append_insert,   (void *)1, 0)));
	ipo_js_set_prop(rt->vm, o, "append",                ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "append",                js_el_append_insert,   (void *)2, 0)));
	ipo_js_set_prop(rt->vm, o, "prepend",               ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "prepend",               js_el_append_insert,   (void *)3, 0)));
	ipo_js_set_prop(rt->vm, o, "remove",                ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "remove",                js_el_remove,          (void *)0, 0)));
	ipo_js_set_prop(rt->vm, o, "removeChild",           ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "removeChild",           js_el_remove,          (void *)1, 0)));
	ipo_js_set_prop(rt->vm, o, "cloneNode",             ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "cloneNode",             js_el_clone,           NULL, 0)));
	ipo_js_set_prop(rt->vm, o, "querySelector",         ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "querySelector",         js_dom_query,          (void *)0, 0)));
	ipo_js_set_prop(rt->vm, o, "querySelectorAll",      ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "querySelectorAll",      js_dom_query,          (void *)1, 0)));
	ipo_js_set_prop(rt->vm, o, "getElementsByTagName",  ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "getElementsByTagName",  js_dom_query,          (void *)3, 0)));
	ipo_js_set_prop(rt->vm, o, "getElementsByClassName",ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "getElementsByClassName",js_dom_query,          (void *)4, 0)));
	ipo_js_set_prop(rt->vm, o, "matches",               ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "matches",               js_dom_query,          (void *)5, 0)));
	ipo_js_set_prop(rt->vm, o, "closest",               ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "closest",               js_dom_query,          (void *)6, 0)));
	ipo_js_set_prop(rt->vm, o, "addEventListener",      ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "addEventListener",      js_el_add_listener,    NULL, 0)));
	ipo_js_set_prop(rt->vm, o, "click",                 ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "click",                 js_el_form_action,     (void *)0, 0)));
	ipo_js_set_prop(rt->vm, o, "focus",                 ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "focus",                 js_el_form_action,     (void *)1, 0)));
	ipo_js_set_prop(rt->vm, o, "blur",                  ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "blur",                  js_el_form_action,     (void *)2, 0)));
	ipo_js_set_prop(rt->vm, o, "submit",                ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "submit",                js_el_form_action,     (void *)3, 0)));
	ipo_js_set_prop(rt->vm, o, "reset",                 ipo_js_obj_val(ipo_js_new_native_fn(rt->vm, "reset",                 js_el_form_action,     (void *)4, 0)));
	return ipo_js_obj_val(o);
}

/* =========================================================================
 * Document, Location, History, Navigator, Timers, Dialogs, Fetch/XHR
 * ========================================================================= */

static int doc_host_get(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t *out) {
	(void)obj;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	dom_node_t *doc = dom_ensure_parsed(rt);

	if (strcmp(key, "title") == 0) {
		dom_node_t *t = dom_find_tag_rec(doc, "title");
		if (t) {
			dbuf_t b; db_init(&b); collect_text_rec(t, &b);
			*out = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
			db_free(&b);
			return 1;
		}
		unsigned char *lt = js_upcall_get_title(fd);
		*out = ipo_js_str_cstr(vm, lt ? (const char *)lt : "");
		if (lt) mem_free(lt);
		return 1;
	}
	if (strcmp(key, "URL") == 0 || strcmp(key, "documentURI") == 0) {
		unsigned char *u = js_upcall_get_location(fd);
		*out = ipo_js_str_cstr(vm, u ? (const char *)u : "");
		if (u) mem_free(u);
		return 1;
	}
	if (strcmp(key, "referrer") == 0) {
		unsigned char *r = js_upcall_get_referrer(fd);
		*out = ipo_js_str_cstr(vm, r ? (const char *)r : "");
		if (r) mem_free(r);
		return 1;
	}
	if (strcmp(key, "cookie") == 0) {
		unsigned char *ck = js_upcall_get_cookies(fd);
		*out = ipo_js_str_cstr(vm, ck ? (const char *)ck : "");
		if (ck) mem_free(ck);
		return 1;
	}
	if (strcmp(key, "readyState") == 0) {
		*out = ipo_js_str_cstr(vm, (fd && fd->script_t >= 0) ? "loading" : "complete");
		return 1;
	}
	if (strcmp(key, "location") == 0) { *out = ipo_js_obj_val(rt->loc_obj); return 1; }
	if (strcmp(key, "documentElement") == 0) { *out = wrap_dom_node(rt, dom_find_tag_rec(doc, "html")); return 1; }
	if (strcmp(key, "head") == 0)            { *out = wrap_dom_node(rt, dom_find_tag_rec(doc, "head")); return 1; }
	if (strcmp(key, "body") == 0)            { *out = wrap_dom_node(rt, dom_find_tag_rec(doc, "body")); return 1; }
	if (strcmp(key, "forms") == 0) {
		ipo_js_obj_t *a = ipo_js_new_array(vm, 0);
		dom_tag_collect(rt, doc, "form", a);
		*out = ipo_js_obj_val(a);
		return 1;
	}
	if (strcmp(key, "links") == 0) {
		ipo_js_obj_t *a = ipo_js_new_array(vm, 0);
		dom_tag_collect(rt, doc, "a", a);
		*out = ipo_js_obj_val(a);
		return 1;
	}
	if (strcmp(key, "images") == 0) {
		ipo_js_obj_t *a = ipo_js_new_array(vm, 0);
		dom_tag_collect(rt, doc, "img", a);
		*out = ipo_js_obj_val(a);
		return 1;
	}
	return 0;
}

static int doc_host_set(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t val) {
	(void)obj;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	if (strcmp(key, "title") == 0) {
		const char *t = ipo_js_cstr(vm, val, NULL);
		js_upcall_set_title(fd, stracpy((unsigned char *)t));
		dom_node_t *doc = dom_ensure_parsed(rt);
		dom_node_t *tn = dom_find_tag_rec(doc, "title");
		if (tn) {
			while (tn->first_child) { dom_node_t *c = tn->first_child; dom_detach(c); dom_free_subtree(rt, c); }
			dom_node_t *txt = dom_alloc_node(rt, DOM_TEXT);
			txt->text = xdup(t);
			dom_append_child(tn, txt);
		}
		ipo_gui_notify_status(fd->ses);
		return 1;
	}
	if (strcmp(key, "cookie") == 0) {
		const char *ck = ipo_js_cstr(vm, val, NULL);
		if (rt->ctx->cookies) js_mem_free(rt->ctx->cookies);
		size_t cl = strlen(ck);
		rt->ctx->cookies = (unsigned char *)js_mem_alloc(cl + 1);
		memcpy(rt->ctx->cookies, ck, cl + 1);
		jsint_set_cookies(fd, cl + 1);
		return 1;
	}
	if (strcmp(key, "location") == 0) {
		const char *u = ipo_js_cstr(vm, val, NULL);
		if (*u && fd && fd->ses) goto_url_utf8(fd->ses, (unsigned char *)u);
		return 1;
	}
	return 0;
}

static ipo_js_val_t js_doc_write(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int ln = (int)(intptr_t)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	dbuf_t b; db_init(&b);
	for (int i = 0; i < argc; i++) {
		size_t slen = 0;
		const char *s = ipo_js_cstr(vm, argv[i], &slen);
		db_add(&b, s, slen);
	}
	if (ln) db_addc(&b, '\n');
	if (jsint_get_active_write_pos(fd) >= 0 && b.len > 0) {
		unsigned char *ms = (unsigned char *)mem_alloc(b.len + 1);
		memcpy(ms, b.data, b.len);
		ms[b.len] = '\0';
		js_upcall_document_write(fd, ms, (int)b.len);
		rt->need_reparse_dom = 1;
	} else if (b.len > 0) {
		dom_node_t *doc = dom_ensure_parsed(rt);
		dom_node_t *bd = dom_find_tag_rec(doc, "body");
		if (bd) {
			parse_html_into(rt, bd, b.data, b.len);
			rt->dom_dirty = 1;
		}
	}
	db_free(&b);
	return ipo_js_undef();
}

static ipo_js_val_t js_doc_create(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int kind = (int)(intptr_t)ud; /* 0=elem, 1=text, 2=comment */
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	const char *arg = (argc >= 1) ? ipo_js_cstr(vm, argv[0], NULL) : "div";
	if (kind == 0) {
		dom_node_t *n = dom_alloc_node(rt, DOM_ELEMENT);
		n->tag = xdupn_lower(arg, strlen(arg));
		return wrap_dom_node(rt, n);
	}
	dom_node_t *n = dom_alloc_node(rt, kind == 1 ? DOM_TEXT : DOM_COMMENT);
	n->text = xdup(argc >= 1 ? arg : "");
	return wrap_dom_node(rt, n);
}

static int loc_host_get(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t *out) {
	(void)obj;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	unsigned char *s = NULL;
	if (strcmp(key, "href") == 0)          s = js_upcall_get_location(fd);
	else if (strcmp(key, "protocol") == 0) s = js_upcall_get_location_protocol(fd);
	else if (strcmp(key, "host") == 0)     s = js_upcall_get_location_host(fd);
	else if (strcmp(key, "hostname") == 0) s = js_upcall_get_location_hostname(fd);
	else if (strcmp(key, "port") == 0)     s = js_upcall_get_location_port(fd);
	else if (strcmp(key, "pathname") == 0) s = js_upcall_get_location_pathname(fd);
	else if (strcmp(key, "search") == 0)   s = js_upcall_get_location_search(fd);
	else if (strcmp(key, "hash") == 0)     s = js_upcall_get_location_hash(fd);
	else return 0;
	*out = ipo_js_str_cstr(vm, s ? (const char *)s : "");
	if (s) mem_free(s);
	return 1;
}

static int loc_host_set(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t val) {
	(void)obj;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	if (strcmp(key, "href") == 0 && fd && fd->ses) {
		const char *u = ipo_js_cstr(vm, val, NULL);
		if (*u) goto_url_utf8(fd->ses, (unsigned char *)u);
		return 1;
	}
	return 0;
}

static ipo_js_val_t js_loc_nav(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int mode = (int)(intptr_t)ud; /* 0=assign/replace, 1=reload, 2=toString */
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	if (mode == 2) {
		unsigned char *u = js_upcall_get_location(fd);
		ipo_js_val_t r = ipo_js_str_cstr(vm, u ? (const char *)u : "");
		if (u) mem_free(u);
		return r;
	}
	if (mode == 1 && fd && fd->ses) { reload(fd->ses, -1); return ipo_js_undef(); }
	if (argc >= 1 && fd && fd->ses) {
		const char *u = ipo_js_cstr(vm, argv[0], NULL);
		if (*u) goto_url_utf8(fd->ses, (unsigned char *)u);
	}
	return ipo_js_undef();
}

static ipo_js_val_t js_hist_nav(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int mode = (int)(intptr_t)ud; /* 0=back, 1=forward, 2=go */
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	int n = (mode == 0) ? -1 : (mode == 1 ? 1 : (argc >= 1 ? ipo_js_to_i32(vm, argv[0]) : 0));
	if (n < 0 && fd && fd->ses) {
		for (int i = 0; i < -n; i++) go_back(fd->ses, 1);
	}
	return ipo_js_undef();
}

/* Dialogs: alert / confirm / prompt */
static ipo_js_val_t js_win_dialog(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int kind = (int)(intptr_t)ud;
	const char *msg = (argc >= 1) ? ipo_js_cstr(vm, argv[0], NULL) : "";
	const char *def = (kind == 2 && argc >= 2) ? ipo_js_cstr(vm, argv[1], NULL) : "";
	char out[512];
	out[0] = '\0';
	int ok = ipo_gui_run_js_dialog(kind,
	                               kind == 0 ? "Alert" : (kind == 1 ? "Confirm" : "Prompt"),
	                               msg, def, out, sizeof(out));
	if (kind == 0) return ipo_js_undef();
	if (kind == 1) return ipo_js_bool(ok);
	return ok ? ipo_js_str_cstr(vm, out) : ipo_js_null();
}

/* Timers: setTimeout / setInterval / clearTimeout / clearInterval */
static void js_timer_fire(void *arg) {
	js_timer_entry_t *te = (js_timer_entry_t *)arg;
	if (!te || te->cancelled || !te->rt || te->rt->destroyed) return;
	ipo_js_runtime_t *rt = te->rt;
	te->links_timer = NULL;

	if (te->fn.type != JS_T_UNDEFINED && ipo_js_is_callable(te->fn)) {
		ipo_js_val_t r = ipo_js_undef();
		ipo_js_call(rt->vm, te->fn, ipo_js_obj_val(ipo_js_global(rt->vm)),
		            te->argc, te->args, &r);
	} else if (te->code_str) {
		ipo_js_eval(rt->vm, te->code_str, strlen(te->code_str), "<timer>", NULL);
	}
	if (ipo_js_has_exception(rt->vm)) {
		ipo_js_val_t ex = ipo_js_get_exception(rt->vm);
		ipo_gui_notify_js_console(ipo_js_cstr(rt->vm, ex, NULL));
		ipo_js_clear_exception(rt->vm);
	}
	if (rt->destroyed) return;
	dom_commit_to_links(rt, 1);

	if (te->is_interval && !te->cancelled && !rt->destroyed) {
		te->links_timer = install_timer((uttime)(te->interval_ms > 0 ? te->interval_ms : 10),
		                                js_timer_fire, te);
	} else {
		te->cancelled = 1;
		if (te->fn.type == JS_T_OBJECT && te->fn.u.obj) ipo_js_gc_unpin(te->fn.u.obj);
	}
}

static ipo_js_val_t js_set_timer(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int is_interval = (int)(intptr_t)ud; /* 0=timeout, 1=interval, 2=raf */
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	if (argc < 1) return ipo_js_num(0);

	js_timer_entry_t *te = (js_timer_entry_t *)calloc(1, sizeof(js_timer_entry_t));
	te->id = ++rt->next_timer_id;
	te->rt = rt;
	te->is_interval = (is_interval == 1);
	int ms = (is_interval == 2) ? 16 : ((argc >= 2) ? ipo_js_to_i32(vm, argv[1]) : 0);
	if (ms < 0) ms = 0;
	if (ms > 300000) ms = 300000;
	te->interval_ms = ms;

	if (ipo_js_is_callable(argv[0])) {
		te->fn = argv[0];
		if (argv[0].type == JS_T_OBJECT && argv[0].u.obj) ipo_js_gc_pin(argv[0].u.obj);
		int ac = (is_interval == 2 || argc <= 2) ? 0 : (argc - 2);
		if (ac > 0) {
			te->args = (ipo_js_val_t *)malloc((size_t)ac * sizeof(ipo_js_val_t));
			for (int i = 0; i < ac; i++) {
				te->args[i] = argv[2 + i];
				if (te->args[i].type == JS_T_OBJECT && te->args[i].u.obj) ipo_js_gc_pin(te->args[i].u.obj);
			}
			te->argc = ac;
		}
	} else {
		te->fn = ipo_js_undef();
		te->code_str = xdup(ipo_js_cstr(vm, argv[0], NULL));
	}

	te->next = rt->timers;
	rt->timers = te;
	te->links_timer = install_timer((uttime)(ms > 0 ? ms : 1), js_timer_fire, te);
	return ipo_js_num((double)te->id);
}

static ipo_js_val_t js_clear_timer(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	if (argc < 1) return ipo_js_undef();
	int id = ipo_js_to_i32(vm, argv[0]);
	for (js_timer_entry_t *te = rt->timers; te; te = te->next) {
		if (te->id == id && !te->cancelled) {
			te->cancelled = 1;
			if (te->links_timer) { kill_timer(te->links_timer); te->links_timer = NULL; }
			if (te->fn.type == JS_T_OBJECT && te->fn.u.obj) ipo_js_gc_unpin(te->fn.u.obj);
			break;
		}
	}
	return ipo_js_undef();
}

/* Base64 btoa / atob */
static const char B64_TAB[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static ipo_js_val_t js_btoa(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_str_cstr(vm, "");
	size_t len = 0;
	const uint8_t *s = (const uint8_t *)ipo_js_cstr(vm, argv[0], &len);
	dbuf_t b; db_init(&b);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t n = ((uint32_t)s[i] << 16) |
		             ((i + 1 < len ? (uint32_t)s[i + 1] : 0) << 8) |
		             (i + 2 < len ? (uint32_t)s[i + 2] : 0);
		db_addc(&b, B64_TAB[(n >> 18) & 63]);
		db_addc(&b, B64_TAB[(n >> 12) & 63]);
		db_addc(&b, i + 1 < len ? B64_TAB[(n >> 6) & 63] : '=');
		db_addc(&b, i + 2 < len ? B64_TAB[n & 63] : '=');
	}
	ipo_js_val_t r = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
	db_free(&b);
	return r;
}

static int b64_dec_val(unsigned char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static ipo_js_val_t js_atob(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	if (argc < 1) return ipo_js_str_cstr(vm, "");
	size_t len = 0;
	const char *s = ipo_js_cstr(vm, argv[0], &len);
	dbuf_t b; db_init(&b);
	uint32_t buf = 0; int bits = 0;
	for (size_t i = 0; i < len; i++) {
		if (isspace((unsigned char)s[i]) || s[i] == '=') continue;
		int v = b64_dec_val((unsigned char)s[i]);
		if (v < 0) continue;
		buf = (buf << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			db_addc(&b, (char)((buf >> bits) & 0xFF));
		}
	}
	ipo_js_val_t r = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
	db_free(&b);
	return r;
}

/* Storage (localStorage / sessionStorage) */
static int storage_get(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t *out) {
	js_kv_item_t **head = (js_kv_item_t **)obj->host_ptr;
	if (!head) return 0;
	if (strcmp(key, "getItem") == 0 || strcmp(key, "setItem") == 0 ||
	    strcmp(key, "removeItem") == 0 || strcmp(key, "clear") == 0) return 0;
	for (js_kv_item_t *it = *head; it; it = it->next) {
		if (strcmp(it->key, key) == 0) { *out = ipo_js_str_cstr(vm, it->val); return 1; }
	}
	return 0;
}

static int storage_set(ipo_js_vm_t *vm, ipo_js_obj_t *obj, const char *key, ipo_js_val_t val) {
	js_kv_item_t **head = (js_kv_item_t **)obj->host_ptr;
	if (!head) return 0;
	const char *vs = ipo_js_cstr(vm, val, NULL);
	for (js_kv_item_t *it = *head; it; it = it->next) {
		if (strcmp(it->key, key) == 0) { free(it->val); it->val = xdup(vs); return 1; }
	}
	js_kv_item_t *ni = (js_kv_item_t *)calloc(1, sizeof(js_kv_item_t));
	ni->key = xdup(key); ni->val = xdup(vs);
	ni->next = *head; *head = ni;
	return 1;
}

static ipo_js_val_t js_storage_op(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int op = (int)(intptr_t)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || !tv.u.obj->host_ptr) return ipo_js_null();
	js_kv_item_t **head = (js_kv_item_t **)tv.u.obj->host_ptr;
	if (op == 0 && argc >= 1) {
		const char *k = ipo_js_cstr(vm, argv[0], NULL);
		for (js_kv_item_t *it = *head; it; it = it->next)
			if (strcmp(it->key, k) == 0) return ipo_js_str_cstr(vm, it->val);
		return ipo_js_null();
	}
	if (op == 1 && argc >= 2) {
		storage_set(vm, tv.u.obj, ipo_js_cstr(vm, argv[0], NULL), argv[1]);
	} else if (op == 2 && argc >= 1) {
		const char *k = ipo_js_cstr(vm, argv[0], NULL);
		for (js_kv_item_t **pp = head; *pp; ) {
			if (strcmp((*pp)->key, k) == 0) {
				js_kv_item_t *d = *pp; *pp = d->next; free(d->key); free(d->val); free(d); break;
			}
			pp = &(*pp)->next;
		}
	} else if (op == 3) {
		while (*head) { js_kv_item_t *d = *head; *head = d->next; free(d->key); free(d->val); free(d); }
	}
	return ipo_js_undef();
}

/* Simple fetch() & XMLHttpRequest using local file / yt_http_stream_t */
static int sync_http_get(const char *abs_url, const char *method, const char *ctype,
                         const char *body, size_t blen, int *out_status, dbuf_t *out_body) {
	if (strncmp(abs_url, "file://", 7) == 0) {
		FILE *f = fopen(abs_url + 7, "rb");
		if (!f) { *out_status = 404; return -1; }
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) db_add(out_body, buf, n);
		fclose(f);
		*out_status = 200;
		return 0;
	}
	yt_http_stream_t st;
	if (yt_http_stream_open(&st, method ? method : "GET", abs_url, ctype, body, blen,
	                        "Mozilla/5.0 (IPO_OS; Links)", false, 0) < 0) {
		*out_status = 0;
		return -1;
	}
	uint8_t buf[8192];
	bool eof = false;
	for (int iter = 0; iter < 4000 && !eof && out_body->len < 2 * 1024 * 1024; iter++) {
		int r = yt_http_stream_read(&st, buf, sizeof(buf), &eof);
		if (r > 0) db_add(out_body, (const char *)buf, (size_t)r);
		else if (r < 0) break;
		else if (!eof) usleep(2000);
	}
	*out_status = st.status_code > 0 ? st.status_code : 200;
	yt_http_stream_close(&st);
	return 0;
}

static ipo_js_val_t js_fetch_resp_text(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)argc; (void)argv;
	int as_json = (int)(intptr_t)ud;
	ipo_js_val_t body = ipo_js_undef();
	if (tv.type == JS_T_OBJECT && tv.u.obj) ipo_js_get_prop(vm, tv.u.obj, "_body", &body);
	ipo_js_obj_t *p = ipo_js_new_promise(vm);
	if (as_json) {
		size_t len = 0; const char *s = ipo_js_cstr(vm, body, &len);
		ipo_js_val_t parsed = ipo_js_undef();
		if (ipo_js_json_parse(vm, s, len, &parsed) == 0) ipo_js_promise_resolve(vm, p, parsed);
		else { ipo_js_promise_reject(vm, p, ipo_js_get_exception(vm)); ipo_js_clear_exception(vm); }
	} else {
		ipo_js_promise_resolve(vm, p, body);
	}
	return ipo_js_obj_val(p);
}

static ipo_js_val_t js_fetch(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	ipo_js_obj_t *p = ipo_js_new_promise(vm);
	if (argc < 1) {
		ipo_js_promise_reject(vm, p, ipo_js_obj_val(ipo_js_new_error(vm, "TypeError", "URL required")));
		return ipo_js_obj_val(p);
	}
	const char *raw_url = ipo_js_cstr(vm, argv[0], NULL);
	unsigned char *base = js_upcall_get_location(fd);
	unsigned char *joined = join_urls(base ? base : (unsigned char *)"", (unsigned char *)raw_url);
	if (base) mem_free(base);

	const char *method = "GET";
	const char *body = NULL;
	size_t blen = 0;
	if (argc >= 2 && argv[1].type == JS_T_OBJECT && argv[1].u.obj) {
		ipo_js_val_t mv = ipo_js_undef(), bv = ipo_js_undef();
		if (ipo_js_get_prop(vm, argv[1].u.obj, "method", &mv) && mv.type == JS_T_STRING)
			method = ipo_js_cstr(vm, mv, NULL);
		if (ipo_js_get_prop(vm, argv[1].u.obj, "body", &bv) && bv.type != JS_T_UNDEFINED && bv.type != JS_T_NULL)
			body = ipo_js_cstr(vm, bv, &blen);
	}

	int status = 0;
	dbuf_t rb; db_init(&rb);
	int rc = sync_http_get(joined ? (const char *)joined : raw_url, method, "application/json",
	                       body, blen, &status, &rb);
	if (rc < 0 && status == 0) {
		if (joined) mem_free(joined);
		db_free(&rb);
		ipo_js_promise_reject(vm, p, ipo_js_obj_val(ipo_js_new_error(vm, "TypeError", "Network request failed")));
		return ipo_js_obj_val(p);
	}

	ipo_js_obj_t *resp = ipo_js_new_object(vm, NULL);
	ipo_js_set_prop(vm, resp, "ok", ipo_js_bool(status >= 200 && status < 300));
	ipo_js_set_prop(vm, resp, "status", ipo_js_num((double)status));
	ipo_js_set_prop(vm, resp, "url", ipo_js_str_cstr(vm, joined ? (const char *)joined : raw_url));
	ipo_js_set_prop(vm, resp, "_body", ipo_js_str_new(vm, rb.data ? rb.data : "", rb.len));
	ipo_js_set_prop(vm, resp, "text", ipo_js_obj_val(ipo_js_new_native_fn(vm, "text", js_fetch_resp_text, (void *)0, 0)));
	ipo_js_set_prop(vm, resp, "json", ipo_js_obj_val(ipo_js_new_native_fn(vm, "json", js_fetch_resp_text, (void *)1, 0)));
	if (joined) mem_free(joined);
	db_free(&rb);
	ipo_js_promise_resolve(vm, p, ipo_js_obj_val(resp));
	return ipo_js_obj_val(p);
}

static ipo_js_val_t js_xhr_open(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	if (tv.type != JS_T_OBJECT || !tv.u.obj || argc < 2) return ipo_js_undef();
	ipo_js_set_prop(vm, tv.u.obj, "_method", argv[0]);
	ipo_js_set_prop(vm, tv.u.obj, "_url", argv[1]);
	ipo_js_set_prop(vm, tv.u.obj, "readyState", ipo_js_num(1));
	return ipo_js_undef();
}

static ipo_js_val_t js_xhr_noop(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)vm; (void)tv; (void)argc; (void)argv; (void)ud;
	return ipo_js_undef();
}

static ipo_js_val_t js_xhr_send(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	if (tv.type != JS_T_OBJECT || !tv.u.obj) return ipo_js_undef();
	ipo_js_obj_t *xhr = tv.u.obj;
	ipo_js_val_t mv = ipo_js_undef(), uv = ipo_js_undef();
	ipo_js_get_prop(vm, xhr, "_method", &mv);
	ipo_js_get_prop(vm, xhr, "_url", &uv);
	unsigned char *base = js_upcall_get_location((struct f_data_c *)rt->ctx->ptr);
	unsigned char *joined = join_urls(base ? base : (unsigned char *)"", (unsigned char *)ipo_js_cstr(vm, uv, NULL));
	if (base) mem_free(base);

	const char *body = NULL; size_t blen = 0;
	if (argc >= 1 && argv[0].type != JS_T_UNDEFINED && argv[0].type != JS_T_NULL)
		body = ipo_js_cstr(vm, argv[0], &blen);
	int status = 0;
	dbuf_t rb; db_init(&rb);
	sync_http_get(joined ? (const char *)joined : ipo_js_cstr(vm, uv, NULL),
	              mv.type == JS_T_STRING ? ipo_js_cstr(vm, mv, NULL) : "GET",
	              "text/plain", body, blen, &status, &rb);
	if (joined) mem_free(joined);
	ipo_js_set_prop(vm, xhr, "status", ipo_js_num((double)status));
	ipo_js_set_prop(vm, xhr, "readyState", ipo_js_num(4));
	ipo_js_set_prop(vm, xhr, "responseText", ipo_js_str_new(vm, rb.data ? rb.data : "", rb.len));
	db_free(&rb);
	ipo_js_val_t cb = ipo_js_undef();
	if (ipo_js_get_prop(vm, xhr, "onreadystatechange", &cb) && ipo_js_is_callable(cb))
		ipo_js_call(vm, cb, tv, 0, NULL, NULL);
	if (ipo_js_get_prop(vm, xhr, "onload", &cb) && ipo_js_is_callable(cb))
		ipo_js_call(vm, cb, tv, 0, NULL, NULL);
	return ipo_js_undef();
}

static ipo_js_val_t js_xhr_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)argc; (void)argv; (void)ud;
	ipo_js_obj_t *o = (tv.type == JS_T_OBJECT && tv.u.obj) ? tv.u.obj : ipo_js_new_object(vm, NULL);
	ipo_js_set_prop(vm, o, "readyState", ipo_js_num(0));
	ipo_js_set_prop(vm, o, "status", ipo_js_num(0));
	ipo_js_set_prop(vm, o, "responseText", ipo_js_str_cstr(vm, ""));
	ipo_js_set_prop(vm, o, "open", ipo_js_obj_val(ipo_js_new_native_fn(vm, "open", js_xhr_open, NULL, 0)));
	ipo_js_set_prop(vm, o, "send", ipo_js_obj_val(ipo_js_new_native_fn(vm, "send", js_xhr_send, NULL, 0)));
	ipo_js_set_prop(vm, o, "setRequestHeader", ipo_js_obj_val(ipo_js_new_native_fn(vm, "setRequestHeader", js_xhr_noop, NULL, 0)));
	return ipo_js_obj_val(o);
}

/* URLSearchParams */
static ipo_js_val_t js_usp_op(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	int op = (int)(intptr_t)ud; /* 0=get, 1=set, 2=has, 3=toString */
	if (tv.type != JS_T_OBJECT || !tv.u.obj) return ipo_js_null();
	ipo_js_val_t mapv = ipo_js_undef();
	if (!ipo_js_get_prop(vm, tv.u.obj, "_map", &mapv) || mapv.type != JS_T_OBJECT || !mapv.u.obj)
		return ipo_js_null();
	if (op == 0 && argc >= 1) {
		ipo_js_val_t r = ipo_js_undef();
		return ipo_js_get_prop(vm, mapv.u.obj, ipo_js_cstr(vm, argv[0], NULL), &r) ? r : ipo_js_null();
	}
	if (op == 1 && argc >= 2) {
		ipo_js_set_prop(vm, mapv.u.obj, ipo_js_cstr(vm, argv[0], NULL), ipo_js_str_cstr(vm, ipo_js_cstr(vm, argv[1], NULL)));
		return ipo_js_undef();
	}
	if (op == 2 && argc >= 1) {
		return ipo_js_bool(ipo_js_has_own_prop(mapv.u.obj, ipo_js_cstr(vm, argv[0], NULL)));
	}
	dbuf_t b; db_init(&b);
	for (size_t i = 0; i < mapv.u.obj->prop_len; i++) {
		if (i > 0) db_addc(&b, '&');
		db_adds(&b, mapv.u.obj->props[i].key->data);
		db_addc(&b, '=');
		db_adds(&b, ipo_js_cstr(vm, mapv.u.obj->props[i].val, NULL));
	}
	ipo_js_val_t r = ipo_js_str_new(vm, b.data ? b.data : "", b.len);
	db_free(&b);
	return r;
}

static ipo_js_val_t js_usp_ctor(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)ud;
	ipo_js_obj_t *o = (tv.type == JS_T_OBJECT && tv.u.obj) ? tv.u.obj : ipo_js_new_object(vm, NULL);
	ipo_js_obj_t *m = ipo_js_new_object(vm, NULL);
	if (argc >= 1 && argv[0].type == JS_T_STRING) {
		const char *s = argv[0].u.str->data;
		if (*s == '?') s++;
		while (*s) {
			const char *amp = strchr(s, '&');
			size_t pl = amp ? (size_t)(amp - s) : strlen(s);
			const char *eq = (const char *)memchr(s, '=', pl);
			if (eq) {
				char *k = xdupn_lower(s, (size_t)(eq - s));
				/* keep original key case */
				memcpy(k, s, (size_t)(eq - s));
				char *v = (char *)malloc(pl - (size_t)(eq + 1 - s) + 1);
				memcpy(v, eq + 1, pl - (size_t)(eq + 1 - s));
				v[pl - (size_t)(eq + 1 - s)] = '\0';
				ipo_js_set_prop(vm, m, k, ipo_js_str_cstr(vm, v));
				free(k); free(v);
			}
			if (!amp) break;
			s = amp + 1;
		}
	}
	ipo_js_set_prop(vm, o, "_map", ipo_js_obj_val(m));
	ipo_js_set_prop(vm, o, "get", ipo_js_obj_val(ipo_js_new_native_fn(vm, "get", js_usp_op, (void *)0, 0)));
	ipo_js_set_prop(vm, o, "set", ipo_js_obj_val(ipo_js_new_native_fn(vm, "set", js_usp_op, (void *)1, 0)));
	ipo_js_set_prop(vm, o, "has", ipo_js_obj_val(ipo_js_new_native_fn(vm, "has", js_usp_op, (void *)2, 0)));
	ipo_js_set_prop(vm, o, "toString", ipo_js_obj_val(ipo_js_new_native_fn(vm, "toString", js_usp_op, (void *)3, 0)));
	return ipo_js_obj_val(o);
}

/* Dispatch synthetic element event (__ipoDispatch(elem_id, 'click')) */
static ipo_js_val_t js_ipo_dispatch(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	if (argc < 2) return ipo_js_bool(1);
	int eid = ipo_js_to_i32(vm, argv[0]);
	const char *type = ipo_js_cstr(vm, argv[1], NULL);
	dom_node_t *doc = dom_ensure_parsed(rt);
	dom_node_t *n = dom_find_by_elem_id_rec(doc, eid);
	if (!n) return ipo_js_bool(1);

	ipo_js_val_t target = wrap_dom_node(rt, n);
	ipo_js_obj_t *ev = ipo_js_new_object(vm, NULL);
	ipo_js_set_prop(vm, ev, "type", ipo_js_str_cstr(vm, type));
	ipo_js_set_prop(vm, ev, "target", target);
	ipo_js_set_prop(vm, ev, "currentTarget", target);
	ipo_js_set_prop(vm, ev, "defaultPrevented", ipo_js_bool(0));
	ipo_js_val_t ev_v = ipo_js_obj_val(ev);

	/* Invoke preventDefault() via native check: eval helper on ev */
	int allow = 1;
	for (dom_listener_t *l = n->listeners; l; l = l->next) {
		if (strcmp(l->type, type) == 0) {
			ipo_js_val_t r = ipo_js_undef();
			ipo_js_call(vm, l->fn, target, 1, &ev_v, &r);
			if (r.type == JS_T_BOOL && r.u.b == 0) allow = 0;
		}
	}
	ipo_js_val_t dp = ipo_js_undef();
	if (ipo_js_get_prop(vm, ev, "defaultPrevented", &dp) && ipo_js_to_bool(dp)) allow = 0;
	return ipo_js_bool(allow);
}

static ipo_js_val_t js_perf_now(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv; (void)argc; (void)argv; (void)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct timeval now;
	gettimeofday(&now, NULL);
	double ms = (double)now.tv_sec * 1000.0 + (double)now.tv_usec / 1000.0;
	return ipo_js_num(ms - rt->start_ms);
}

static ipo_js_val_t js_scroll_to(ipo_js_vm_t *vm, ipo_js_val_t tv, int argc, const ipo_js_val_t *argv, void *ud) {
	(void)tv;
	int is_by = (int)(intptr_t)ud;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ipo_js_vm_host_ctx(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;
	if (!fd || !fd->vs) return ipo_js_undef();
	int x = 0, y = 0;
	if (argc == 1 && argv[0].type == JS_T_OBJECT && argv[0].u.obj) {
		ipo_js_val_t vx = ipo_js_undef(), vy = ipo_js_undef();
		if (ipo_js_get_prop(vm, argv[0].u.obj, "left", &vx)) x = ipo_js_to_i32(vm, vx);
		if (ipo_js_get_prop(vm, argv[0].u.obj, "top", &vy))  y = ipo_js_to_i32(vm, vy);
	} else if (argc >= 2) {
		x = ipo_js_to_i32(vm, argv[0]);
		y = ipo_js_to_i32(vm, argv[1]);
	}
	if (is_by) { fd->vs->view_posx += x; fd->vs->view_pos += y; }
	else { fd->vs->view_posx = x; fd->vs->view_pos = y; }
	if (fd->vs->view_posx < 0) fd->vs->view_posx = 0;
	if (fd->vs->view_pos < 0) fd->vs->view_pos = 0;
	draw_fd(fd);
	return ipo_js_undef();
}

static void runtime_console_hook(void *host_ctx, int level, const char *msg) {
	(void)host_ctx; (void)level;
	ipo_gui_notify_js_console(msg);
}

static void setup_browser_globals(ipo_js_runtime_t *rt) {
	ipo_js_vm_t *vm = rt->vm;
	ipo_js_obj_t *g = ipo_js_global(vm);
	struct f_data_c *fd = (struct f_data_c *)rt->ctx->ptr;

	ipo_js_vm_set_console_hook(vm, runtime_console_hook);
	ipo_js_set_prop(vm, g, "window", ipo_js_obj_val(g));
	ipo_js_set_prop(vm, g, "self",   ipo_js_obj_val(g));
	ipo_js_set_prop(vm, g, "top",    ipo_js_obj_val(g));
	ipo_js_set_prop(vm, g, "parent", ipo_js_obj_val(g));

	/* navigator */
	ipo_js_obj_t *nav = ipo_js_new_object(vm, NULL);
	unsigned char *ua = js_upcall_get_useragent(fd);
	unsigned char *an = js_upcall_get_appname();
	unsigned char *ac = js_upcall_get_appcodename();
	unsigned char *av = js_upcall_get_appversion();
	ipo_js_set_prop(vm, nav, "userAgent",     ipo_js_str_cstr(vm, ua ? (const char *)ua : "Links (IPO_OS)"));
	ipo_js_set_prop(vm, nav, "appName",       ipo_js_str_cstr(vm, an ? (const char *)an : "Links"));
	ipo_js_set_prop(vm, nav, "appCodeName",   ipo_js_str_cstr(vm, ac ? (const char *)ac : "Links"));
	ipo_js_set_prop(vm, nav, "appVersion",    ipo_js_str_cstr(vm, av ? (const char *)av : "2.22"));
	ipo_js_set_prop(vm, nav, "platform",      ipo_js_str_cstr(vm, "IPO_OS x86"));
	ipo_js_set_prop(vm, nav, "language",      ipo_js_str_cstr(vm, "en-US"));
	ipo_js_set_prop(vm, nav, "cookieEnabled", ipo_js_bool(1));
	ipo_js_set_prop(vm, nav, "onLine",        ipo_js_bool(1));
	if (ua) mem_free(ua);
	if (an) mem_free(an);
	if (ac) mem_free(ac);
	if (av) mem_free(av);
	ipo_js_set_prop(vm, g, "navigator", ipo_js_obj_val(nav));

	/* location */
	ipo_js_obj_t *loc = ipo_js_new_object(vm, NULL);
	loc->host_tag = HTAG_LOCATION;
	loc->host_get = loc_host_get;
	loc->host_set = loc_host_set;
	ipo_js_set_prop(vm, loc, "assign",   ipo_js_obj_val(ipo_js_new_native_fn(vm, "assign",   js_loc_nav, (void *)0, 0)));
	ipo_js_set_prop(vm, loc, "replace",  ipo_js_obj_val(ipo_js_new_native_fn(vm, "replace",  js_loc_nav, (void *)0, 0)));
	ipo_js_set_prop(vm, loc, "reload",   ipo_js_obj_val(ipo_js_new_native_fn(vm, "reload",   js_loc_nav, (void *)1, 0)));
	ipo_js_set_prop(vm, loc, "toString", ipo_js_obj_val(ipo_js_new_native_fn(vm, "toString", js_loc_nav, (void *)2, 0)));
	rt->loc_obj = loc;
	ipo_js_set_prop(vm, g, "location", ipo_js_obj_val(loc));

	/* history */
	ipo_js_obj_t *hist = ipo_js_new_object(vm, NULL);
	ipo_js_set_prop(vm, hist, "length", ipo_js_num((double)js_upcall_get_history_length(fd)));
	ipo_js_set_prop(vm, hist, "back",    ipo_js_obj_val(ipo_js_new_native_fn(vm, "back",    js_hist_nav, (void *)0, 0)));
	ipo_js_set_prop(vm, hist, "forward", ipo_js_obj_val(ipo_js_new_native_fn(vm, "forward", js_hist_nav, (void *)1, 0)));
	ipo_js_set_prop(vm, hist, "go",      ipo_js_obj_val(ipo_js_new_native_fn(vm, "go",      js_hist_nav, (void *)2, 0)));
	ipo_js_set_prop(vm, g, "history", ipo_js_obj_val(hist));

	/* screen & viewport dimensions */
	ipo_js_obj_t *scr = ipo_js_new_object(vm, NULL);
	ipo_js_set_prop(vm, scr, "width",       ipo_js_num(800.0));
	ipo_js_set_prop(vm, scr, "height",      ipo_js_num(600.0));
	ipo_js_set_prop(vm, scr, "availWidth",  ipo_js_num(800.0));
	ipo_js_set_prop(vm, scr, "availHeight", ipo_js_num(600.0));
	ipo_js_set_prop(vm, scr, "colorDepth",  ipo_js_num(32.0));
	ipo_js_set_prop(vm, g, "screen", ipo_js_obj_val(scr));
	ipo_js_set_prop(vm, g, "innerWidth",  ipo_js_num(800.0));
	ipo_js_set_prop(vm, g, "innerHeight", ipo_js_num(520.0));
	ipo_js_set_prop(vm, g, "devicePixelRatio", ipo_js_num(1.0));

	/* document */
	ipo_js_obj_t *doc = ipo_js_new_object(vm, NULL);
	doc->host_tag = HTAG_DOCUMENT;
	doc->host_get = doc_host_get;
	doc->host_set = doc_host_set;
	ipo_js_set_prop(vm, doc, "write",                  ipo_js_obj_val(ipo_js_new_native_fn(vm, "write",                  js_doc_write,      (void *)0, 0)));
	ipo_js_set_prop(vm, doc, "writeln",                ipo_js_obj_val(ipo_js_new_native_fn(vm, "writeln",                js_doc_write,      (void *)1, 0)));
	ipo_js_set_prop(vm, doc, "createElement",          ipo_js_obj_val(ipo_js_new_native_fn(vm, "createElement",          js_doc_create,     (void *)0, 0)));
	ipo_js_set_prop(vm, doc, "createTextNode",         ipo_js_obj_val(ipo_js_new_native_fn(vm, "createTextNode",         js_doc_create,     (void *)1, 0)));
	ipo_js_set_prop(vm, doc, "createComment",          ipo_js_obj_val(ipo_js_new_native_fn(vm, "createComment",          js_doc_create,     (void *)2, 0)));
	ipo_js_set_prop(vm, doc, "getElementById",         ipo_js_obj_val(ipo_js_new_native_fn(vm, "getElementById",         js_dom_query,      (void *)2, 0)));
	ipo_js_set_prop(vm, doc, "querySelector",          ipo_js_obj_val(ipo_js_new_native_fn(vm, "querySelector",          js_dom_query,      (void *)0, 0)));
	ipo_js_set_prop(vm, doc, "querySelectorAll",       ipo_js_obj_val(ipo_js_new_native_fn(vm, "querySelectorAll",       js_dom_query,      (void *)1, 0)));
	ipo_js_set_prop(vm, doc, "getElementsByTagName",   ipo_js_obj_val(ipo_js_new_native_fn(vm, "getElementsByTagName",   js_dom_query,      (void *)3, 0)));
	ipo_js_set_prop(vm, doc, "getElementsByClassName", ipo_js_obj_val(ipo_js_new_native_fn(vm, "getElementsByClassName", js_dom_query,      (void *)4, 0)));
	ipo_js_set_prop(vm, doc, "addEventListener",       ipo_js_obj_val(ipo_js_new_native_fn(vm, "addEventListener",       js_el_add_listener, NULL, 0)));
	rt->doc_obj = doc;
	ipo_js_set_prop(vm, g, "document", ipo_js_obj_val(doc));

	/* storage */
	ipo_js_obj_t *ls = ipo_js_new_object(vm, NULL);
	ls->host_tag = HTAG_STORAGE; ls->host_ptr = &rt->local_storage;
	ls->host_get = storage_get; ls->host_set = storage_set;
	ipo_js_set_prop(vm, ls, "getItem",    ipo_js_obj_val(ipo_js_new_native_fn(vm, "getItem",    js_storage_op, (void *)0, 0)));
	ipo_js_set_prop(vm, ls, "setItem",    ipo_js_obj_val(ipo_js_new_native_fn(vm, "setItem",    js_storage_op, (void *)1, 0)));
	ipo_js_set_prop(vm, ls, "removeItem", ipo_js_obj_val(ipo_js_new_native_fn(vm, "removeItem", js_storage_op, (void *)2, 0)));
	ipo_js_set_prop(vm, ls, "clear",      ipo_js_obj_val(ipo_js_new_native_fn(vm, "clear",      js_storage_op, (void *)3, 0)));
	ipo_js_set_prop(vm, g, "localStorage", ipo_js_obj_val(ls));

	ipo_js_obj_t *ss = ipo_js_new_object(vm, NULL);
	ss->host_tag = HTAG_STORAGE; ss->host_ptr = &rt->session_storage;
	ss->host_get = storage_get; ss->host_set = storage_set;
	ipo_js_set_prop(vm, ss, "getItem",    ipo_js_obj_val(ipo_js_new_native_fn(vm, "getItem",    js_storage_op, (void *)0, 0)));
	ipo_js_set_prop(vm, ss, "setItem",    ipo_js_obj_val(ipo_js_new_native_fn(vm, "setItem",    js_storage_op, (void *)1, 0)));
	ipo_js_set_prop(vm, ss, "removeItem", ipo_js_obj_val(ipo_js_new_native_fn(vm, "removeItem", js_storage_op, (void *)2, 0)));
	ipo_js_set_prop(vm, ss, "clear",      ipo_js_obj_val(ipo_js_new_native_fn(vm, "clear",      js_storage_op, (void *)3, 0)));
	ipo_js_set_prop(vm, g, "sessionStorage", ipo_js_obj_val(ss));

	/* performance */
	ipo_js_obj_t *perf = ipo_js_new_object(vm, NULL);
	ipo_js_set_prop(vm, perf, "now", ipo_js_obj_val(ipo_js_new_native_fn(vm, "now", js_perf_now, NULL, 0)));
	ipo_js_set_prop(vm, g, "performance", ipo_js_obj_val(perf));

	/* Window functions */
	ipo_js_set_prop(vm, g, "alert",                 ipo_js_obj_val(ipo_js_new_native_fn(vm, "alert",                 js_win_dialog,      (void *)0, 0)));
	ipo_js_set_prop(vm, g, "confirm",               ipo_js_obj_val(ipo_js_new_native_fn(vm, "confirm",               js_win_dialog,      (void *)1, 0)));
	ipo_js_set_prop(vm, g, "prompt",                ipo_js_obj_val(ipo_js_new_native_fn(vm, "prompt",                js_win_dialog,      (void *)2, 0)));
	ipo_js_set_prop(vm, g, "setTimeout",            ipo_js_obj_val(ipo_js_new_native_fn(vm, "setTimeout",            js_set_timer,       (void *)0, 0)));
	ipo_js_set_prop(vm, g, "setInterval",           ipo_js_obj_val(ipo_js_new_native_fn(vm, "setInterval",           js_set_timer,       (void *)1, 0)));
	ipo_js_set_prop(vm, g, "requestAnimationFrame", ipo_js_obj_val(ipo_js_new_native_fn(vm, "requestAnimationFrame", js_set_timer,       (void *)2, 0)));
	ipo_js_set_prop(vm, g, "clearTimeout",          ipo_js_obj_val(ipo_js_new_native_fn(vm, "clearTimeout",          js_clear_timer,     NULL, 0)));
	ipo_js_set_prop(vm, g, "clearInterval",         ipo_js_obj_val(ipo_js_new_native_fn(vm, "clearInterval",         js_clear_timer,     NULL, 0)));
	ipo_js_set_prop(vm, g, "cancelAnimationFrame",  ipo_js_obj_val(ipo_js_new_native_fn(vm, "cancelAnimationFrame",  js_clear_timer,     NULL, 0)));
	ipo_js_set_prop(vm, g, "btoa",                  ipo_js_obj_val(ipo_js_new_native_fn(vm, "btoa",                  js_btoa,            NULL, 0)));
	ipo_js_set_prop(vm, g, "atob",                  ipo_js_obj_val(ipo_js_new_native_fn(vm, "atob",                  js_atob,            NULL, 0)));
	ipo_js_set_prop(vm, g, "scrollTo",              ipo_js_obj_val(ipo_js_new_native_fn(vm, "scrollTo",              js_scroll_to,       (void *)0, 0)));
	ipo_js_set_prop(vm, g, "scrollBy",              ipo_js_obj_val(ipo_js_new_native_fn(vm, "scrollBy",              js_scroll_to,       (void *)1, 0)));
	ipo_js_set_prop(vm, g, "fetch",                 ipo_js_obj_val(ipo_js_new_native_fn(vm, "fetch",                 js_fetch,           NULL, 0)));
	ipo_js_set_prop(vm, g, "XMLHttpRequest",        ipo_js_obj_val(ipo_js_new_native_fn(vm, "XMLHttpRequest",        js_xhr_ctor,        NULL, 1)));
	ipo_js_set_prop(vm, g, "URLSearchParams",       ipo_js_obj_val(ipo_js_new_native_fn(vm, "URLSearchParams",       js_usp_ctor,        NULL, 1)));
	ipo_js_set_prop(vm, g, "addEventListener",      ipo_js_obj_val(ipo_js_new_native_fn(vm, "addEventListener",      js_el_add_listener, NULL, 0)));
	ipo_js_set_prop(vm, g, "__ipoDispatch",         ipo_js_obj_val(ipo_js_new_native_fn(vm, "__ipoDispatch",         js_ipo_dispatch,    NULL, 0)));
}

/* =========================================================================
 * Links Execution Hooks (create / destroy / execute via 0ms timer)
 * ========================================================================= */

static int url_hex_val(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static char *decode_percent_js(const char *s, size_t len, size_t *out_len) {
	char *d = (char *)malloc(len + 1);
	size_t j = 0;
	int changed = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '%' && i + 2 < len) {
			int a = url_hex_val(s[i + 1]), b = url_hex_val(s[i + 2]);
			if (a >= 0 && b >= 0) {
				d[j++] = (char)((a << 4) | b);
				i += 2;
				changed = 1;
				continue;
			}
		}
		d[j++] = s[i];
	}
	d[j] = '\0';
	if (!changed) { free(d); return NULL; }
	*out_len = j;
	return d;
}

static void js_exec_timer_cb(void *arg) {
	js_context *ctx = (js_context *)arg;
	if (!ctx) return;
	ctx->exec_timer = NULL;
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ctx->runtime;
	if (!rt || rt->destroyed) return;

	void (*done_cb)(void *) = rt->pending_done_cb;
	unsigned char *code = rt->pending_code;
	int len = rt->pending_len;
	rt->pending_done_cb = NULL;
	rt->pending_code = NULL;
	rt->pending_len = 0;

	struct f_data_c *fd = (struct f_data_c *)ctx->ptr;
	int cur_wp = jsint_get_active_write_pos(fd);
	if (cur_wp >= 0) {
		rt->active_script_ord++;
	}

	ctx->running = 1;
	ctx->zaplatim = 1;
	dom_ensure_parsed(rt);

	/* Trim trailing NULs if any */
	while (len > 0 && code[len - 1] == '\0') len--;

	ipo_js_val_t res = ipo_js_undef();
	int rc = ipo_js_eval(rt->vm, (const char *)code, (size_t)len, "<script>", &res);

	/* If javascript: URL contained URL %XX escapes and failed syntax check, retry decoded */
	if (rc < 0 && cur_wp < 0) {
		size_t dlen = 0;
		char *dec = decode_percent_js((const char *)code, (size_t)len, &dlen);
		if (dec) {
			ipo_js_clear_exception(rt->vm);
			rc = ipo_js_eval(rt->vm, dec, dlen, "<javascript:>", &res);
			free(dec);
		}
	}

	if (rc < 0 && ipo_js_has_exception(rt->vm)) {
		ipo_js_val_t ex = ipo_js_get_exception(rt->vm);
		const char *emsg = ipo_js_cstr(rt->vm, ex, NULL);
		fprintf(stderr, "[JS Error] %s\n", emsg);
		ipo_gui_notify_js_console(emsg);
		ipo_js_clear_exception(rt->vm);
	} else {
		/* Links event / javascript: convention: `return false` sets zaplatim = 0 */
		if (res.type == JS_T_BOOL && res.u.b == 0) ctx->zaplatim = 0;
		/* If `javascript:expr` evaluated to a scalar and wasn't void/write, show status if user ran it */
		if (cur_wp == -1 && !rt->dom_dirty && res.type != JS_T_UNDEFINED && res.type != JS_T_BOOL) {
			ipo_gui_notify_js_console(ipo_js_cstr(rt->vm, res, NULL));
		}
	}

	/* Also handle `return expr;` inside inline event attributes like `onclick="return check();"` */
	free(code);
	dom_commit_to_links(rt, 0);
	ctx->running = 0;

	if (done_cb && ctx->ptr) done_cb(ctx->ptr);

	/* After all page scripts complete, fire DOMContentLoaded / load listeners */
	if (!rt->destroyed && fd && fd->script_t == -1 && rt->win_listeners) {
		dom_listener_t *l = rt->win_listeners;
		rt->win_listeners = NULL;
		for (dom_listener_t *cur = l; cur; cur = cur->next) {
			if (ci_eq(cur->type, "DOMContentLoaded") || ci_eq(cur->type, "load")) {
				ipo_js_val_t ev_res = ipo_js_undef();
				ipo_js_call(rt->vm, cur->fn, ipo_js_obj_val(ipo_js_global(rt->vm)), 0, NULL, &ev_res);
			}
		}
		dom_free_listeners(rt, l);
		dom_commit_to_links(rt, 1);
	}
}

struct javascript_context *js_create_context(void *ptr, long doc_id) {
	js_context *ctx = (js_context *)calloc(1, sizeof(js_context));
	if (!ctx) return NULL;
	ctx->ptr = ptr;
	ctx->js_id = doc_id;
	ctx->zaplatim = 1;

	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)calloc(1, sizeof(ipo_js_runtime_t));
	if (!rt) { free(ctx); return NULL; }
	rt->ctx = ctx;
	rt->vm = ipo_js_vm_new(rt, js_fun_depth > 0 ? js_fun_depth : 100, 2000000);
	if (!rt->vm) { free(rt); free(ctx); return NULL; }

	struct timeval tv;
	gettimeofday(&tv, NULL);
	rt->start_ms = (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
	ctx->runtime = rt;

	setup_browser_globals(rt);
	return ctx;
}

void js_destroy_context(struct javascript_context *ctx) {
	if (!ctx) return;
	js_spec_vykill_timer(ctx, 0);
	if (ctx->cookies) {
		js_mem_free(ctx->cookies);
		ctx->cookies = NULL;
	}
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ctx->runtime;
	if (rt) {
		rt->destroyed = 1;
		free(rt->pending_code);
		rt->pending_code = NULL;
		for (js_timer_entry_t *te = rt->timers; te; ) {
			js_timer_entry_t *nx = te->next;
			te->cancelled = 1;
			if (te->links_timer) kill_timer(te->links_timer);
			free(te->code_str);
			free(te->args);
			free(te);
			te = nx;
		}
		rt->timers = NULL;
		dom_free_listeners(rt, rt->win_listeners);
		rt->win_listeners = NULL;
		while (rt->local_storage) {
			js_kv_item_t *d = rt->local_storage;
			rt->local_storage = d->next;
			free(d->key); free(d->val); free(d);
		}
		while (rt->session_storage) {
			js_kv_item_t *d = rt->session_storage;
			rt->session_storage = d->next;
			free(d->key); free(d->val); free(d);
		}
		if (rt->doc) {
			dom_free_subtree(rt, rt->doc);
			rt->doc = NULL;
		}
		if (rt->vm) {
			ipo_js_vm_free(rt->vm);
			rt->vm = NULL;
		}
		free(rt);
		ctx->runtime = NULL;
	}
	free(ctx);
}

void js_execute_code(struct javascript_context *ctx, unsigned char *code, int len, void (*callback)(void *)) {
	if (!ctx || !ctx->runtime) {
		if (callback && ctx && ctx->ptr) callback(ctx->ptr);
		return;
	}
	ipo_js_runtime_t *rt = (ipo_js_runtime_t *)ctx->runtime;
	js_spec_vykill_timer(ctx, 0);
	free(rt->pending_code);

	if (len < 0) len = code ? (int)strlen((const char *)code) : 0;
	rt->pending_code = (unsigned char *)malloc((size_t)len + 1);
	if (rt->pending_code) {
		if (len > 0 && code) memcpy(rt->pending_code, code, (size_t)len);
		rt->pending_code[len] = '\0';
	}
	rt->pending_len = len;
	rt->pending_done_cb = callback;
	ctx->exec_timer = install_timer(0, js_exec_timer_cb, ctx);
}

#endif /* JS */
