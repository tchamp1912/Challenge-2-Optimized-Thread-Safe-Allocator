#ifndef XMALLOC_H
#define XMALLOC_H

#include <stddef.h>

void* xmalloc(size_t bytes);
void  xfree(void* ptr);
void* xrealloc(void* item, size_t size);

#endif
