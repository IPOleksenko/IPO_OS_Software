#ifndef LINKS_JS_STRUCT_H
#define LINKS_JS_STRUCT_H

#include <stddef.h>

typedef struct javascript_context {
	void *ptr;              /* struct f_data_c * */
	long js_id;
	unsigned char *cookies; /* allocated via js_mem_alloc/js_mem_free */
	int zaplatim;           /* 1 = allow default link/form action, 0 = cancel */
	int running;
	void *exec_timer;       /* struct timer * */
	void *runtime;          /* ipo_js_runtime_t * */
	long depth_guard;
} js_context;

#endif
