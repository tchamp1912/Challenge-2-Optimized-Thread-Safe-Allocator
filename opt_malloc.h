#ifndef OPT_MALLOC_H
#define OPT_MALLOC_H


void* xmalloc(size_t size);
void xfree(void* item);
void* realloc(void* item, size_t size);

#endif
