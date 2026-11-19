#ifndef LINKS_JS_IPRET_H
#define LINKS_JS_IPRET_H

#include <stddef.h>
#include "struct.h"

extern long js_zaflaknuto_pameti;

void *js_mem_alloc(size_t size);
void js_mem_free(void *ptr);
unsigned char *stracpy1(const unsigned char *src);
void js_spec_vykill_timer(js_context *ctx, int type);

#endif
