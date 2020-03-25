
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <assert.h>

#include "hwx_malloc.h"



// #include "llist.h"


/////////////////////////////////////////////////////////////////////
////////////////////////////// hmalloc.c ////////////////////////////

/*
typedef struct hm_stats {
  long pages_mapped;
  long pages_unmapped;
  long chunks_allocated;
  long chunks_freed;
  long free_length;
} hm_stats;
*/


const size_t PAGE_SIZE = 4096;
static hm_stats stats; // This initializes the stats to 0.

static llist_node* free_list_head = NULL;

pthread_mutex_t free_list_lock;
int free_list_lock_initialized = 0;


long
free_list_length()
{
    return llist_length(free_list_head);
}

void
free_list_insert(llist_node* node)
{
    free_list_head = llist_insert(node, free_list_head);
}

hm_stats*
hgetstats()
{
    stats.free_length = free_list_length();
    return &stats;
}

void
hprintstats()
{
    stats.free_length = free_list_length();
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", stats.pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", stats.chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", stats.chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", stats.free_length);
}

static
size_t
div_up(size_t xx, size_t yy)
{
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx)
    {
        return zz;
    }
    else
    {
        return zz + 1;
    }
}

void*
xmalloc(size_t size)
{
    if (!free_list_lock_initialized) {
        pthread_mutex_init(&free_list_lock, 0);
        free_list_lock_initialized = 1;
    }

    stats.chunks_allocated += 1;
    size += sizeof(size_t);

    // Use the start of the block to store its size.
    // Return a pointer to the block after the size field.
    void* new_bstart;
    size_t new_bsize;

    // Requests with (B < 1 page = 4096 bytes)
    if (size < PAGE_SIZE)
    {

        pthread_mutex_lock(&free_list_lock);

        //See if there’s a big enough block on the free list. If so, select the first one ...
        llist_node* node = xmallocHlp_get_free_block(size);

        //  ... and remove it from the list.
        if (node != NULL)
        {
            new_bstart = (void*)node;
            new_bsize = node->size;
        }
        else // If you don’t have a block, mmap a new block (1 page)
        {
            new_bsize = PAGE_SIZE;
            new_bstart = mmap(NULL, new_bsize, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
            assert(new_bstart != 0);
            stats.pages_mapped += 1;
        }

        // If the block is bigger than the request, and the leftover is big enough to
        // store a free list cell, return the extra to the free list.
        if (new_bsize - size > sizeof(llist_node))
        {
            llist_node* new_block = (llist_node*)(new_bstart + size);
            new_block->size = new_bsize - size;
            free_list_insert(new_block);
            new_bsize = size;
        }

        pthread_mutex_unlock(&free_list_lock);
    }
    else // Requests with (B >= 1 page = 4096 bytes):
    {
        size_t num_pages = div_up(size, PAGE_SIZE); // Calculate the number of pages needed for this block.
        new_bsize = PAGE_SIZE * num_pages; // // Allocate that many pages
        new_bstart = mmap(NULL, new_bsize, PROT_READ | PROT_WRITE, // with mmap
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

        assert(new_bstart != 0);
        stats.pages_mapped += num_pages;
    }

    *((size_t*)new_bstart) = new_bsize;
    return new_bstart + sizeof(size_t);
}

// See if there’s a big enough block on the free list. If so, select the first one, remove it from the list, and return int
// if not, return null
llist_node*
xmallocHlp_get_free_block(size_t min_size)
{
    if (free_list_head == NULL)
    {
        return NULL;
    }

    llist_node* nn = free_list_head;

    //head is big enough to use
    if (free_list_head->size >= min_size)
    {
        free_list_head = free_list_head->next;
        return nn;
    }

    llist_node* pp;

    //iterate through the rest of the nodes
    do
    {
        pp = nn; // need to update prev b4 iterating
        nn = nn->next;

        //stop if end of list OR found block big enough
    } while (nn != NULL && nn->size < min_size);

    if (nn != NULL) // didn't reach end of list
    {
        pp->next = nn->next;
    }

    return nn;
}

void
xfree(void* item)
{

    stats.chunks_freed += 1;

    void* bstart = item - sizeof(size_t);
    size_t bsize = *((size_t*)bstart);

    // If the block is < 1 page
    if (bsize < PAGE_SIZE)
    {
        pthread_mutex_lock(&free_list_lock);
        free_list_insert((llist_node*)bstart); // then stick it on the free list.
        pthread_mutex_unlock(&free_list_lock);

    }
    else
    {
        int rv = munmap(bstart, bsize); // then munmap it.
        assert(rv == 0);
        stats.pages_unmapped += bsize / PAGE_SIZE;
    }
}

void *
xrealloc(void *item, size_t size) {

    // if you're trying to reallocate the head,
    // you are trying to realloc the entire list
    if (item == NULL) {
        return xmalloc(size);
    }

    size_t new_free;
    void *new_ptr;

    // size of memory block to realloc:

    /*
     * I was gonna see if I could make this void* and use sizeof(block_header) to keep
     * track of the how much memory is being realloced....
     * but then i accidently settled on this, and im not sure why by substracting
     * the size a normal size_t works
     */
    llist_node *block_header = ((llist_node *) (item - (sizeof(size_t))));

    // less memory is required
    if (block_header->size > size + sizeof(llist_node)) {

        llist_node *free_mem;

        // new size of memory to add to free list
        new_free = block_header->size - size;

        // set block size to new realloc size
        block_header->size = size;

        // increment pointer value by new size
        free_mem = (llist_node *) (item + size);

        // subtract mapping overhead
        new_free -= sizeof(llist_node);
        free_mem->size = new_free;

        pthread_mutex_lock(&free_list_lock);
        // if free list is empty add free memory to it
        if (free_list_head == NULL) {
            free_mem->next = NULL;
            free_list_head = free_mem;

        }
        // if not first element add to free list
        else {
            llist_insert(free_list_head, free_mem);
        }
        pthread_mutex_unlock(&free_list_lock);


        return item;
    }
    // more memory is required
    else if (block_header->size < size) {

        // allocate new memory
        new_ptr = xmalloc(size);

        // copy old memory to new memory
        memcpy(new_ptr, item, block_header->size);

        // free old memory
        xfree(item);

        return new_ptr;

    }
    // edge case if they are equal
    else {
        // return old pointer
        return item;
    }
}

/////////////////////////////////////////////////////////////////////
////////////////////////////// llist.c //////////////////////////////

// based on generic linked list code and lecture code

/*
typedef struct llist_node {
    size_t size;     // size of one node (which represents one free block)
    struct free_list_node* next; // NULL if end of list
} free_list_node;
*/

long
llist_length(llist_node* list_head)
{
    long length = 0;
    for (llist_node* node = list_head; node != NULL; node = node->next)
    {
        length++;
    }
    return length;
}

//inserts a given node and coelesces it when necessary
llist_node*
llist_insert(llist_node* to_insert, llist_node* list_head)
{
    if (list_head == NULL)
    {
        to_insert->next = NULL;
        return to_insert;
    }

    // inserting before (or at if the size is big enough) the head
    if (to_insert < list_head)
    {
        // Any two adjacent blocks on the free list get coalesced (joined together) into one bigger block.
        // This is the special case where the head needs to be coalesced, by something in front of it
        if ((void*)to_insert + to_insert->size == list_head)
        {
            //at head, in front
            to_insert->size += list_head->size; // add the list to the node
            to_insert->next = list_head->next; // replace the head with the node
            return to_insert;
        }

        // replace the head with the node, but keep the head (don't coalesce)
        to_insert->next = list_head;
        return to_insert;
    }

    // Any two adjacent blocks on the free list get coalesced (joined together) into one bigger block.
    if ((void*)list_head + list_head->size == to_insert)
    {
        //at head, behind
        list_head->size += to_insert->size;
        return llist_insert(list_head, list_head->next); // head is now bigger, need to shift the list down
    }

    // insert after head
    list_head->next = llist_insert(to_insert, list_head->next);
    return list_head;
}


