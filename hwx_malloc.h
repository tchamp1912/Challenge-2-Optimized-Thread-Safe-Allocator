#ifndef HMALLOC_H
#define HMALLOC_H

// Husky Malloc Interface
// cs3650 Starter Code

/////////////////////////////////////////////////////////////////////
////////////////////////////// llist.h //////////////////////////////

// based on generic linked list code and lecture code

typedef struct llist_node {
    size_t size;     // curr size of one free block
    struct llist_node* next; // NULL if end of list
} llist_node;

long llist_length(llist_node* list_head);
llist_node* llist_insert(llist_node* to_insert, llist_node* list_head);
// void print_list(llist_node* list_head); // was giving clang errors and wasn't needed

/////////////////////////////////////////////////////////////////////
////////////////////////////// hmalloc.h //////////////////////////////

typedef struct hm_stats {
    long pages_mapped;
    long pages_unmapped;
    long chunks_allocated;
    long chunks_freed;
    long free_length;
} hm_stats;

hm_stats* hgetstats();
void hprintstats();
void* xmalloc(size_t size);
void xfree(void* item);
llist_node* xmallocHlp_get_free_block(size_t min_size);



#endif

