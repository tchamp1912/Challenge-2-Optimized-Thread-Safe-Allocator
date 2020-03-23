
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "hwx_malloc.h"

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
static node_t *FREE_LIST = NULL;
static int MUNMAP = 1;
pthread_mutex_t free_list_lock;
int mutex_init_flag = 0;

long
free_list_length() {
    // Calculate the length of the free list.
    node_t *curr_node = FREE_LIST;
    size_t length = 0;

    while (curr_node != NULL) {
        // if node isnt null increment
        length++;
        curr_node = curr_node->next;
    }

    return (long) length;
}

hm_stats *
hgetstats() {
    stats.free_length = free_list_length();
    return &stats;
}

void
hprintstats() {
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
div_up(size_t xx, size_t yy) {
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx) {
        return zz;
    } else {
        return zz + 1;
    }
}


void
_coalescing_check(node_t *curr_node) {

    if (curr_node != NULL)
        _coalescing_check(curr_node->next);
    else
        return;

    // coalescing check
    if ((((size_t) curr_node) + curr_node->size + sizeof(node_t)) == ((size_t) curr_node->next) && curr_node->next) {

        // increment node size by next node
        curr_node->size += curr_node->next->size + sizeof(node_t);

        // change node next
        curr_node->next = curr_node->next->next;

    }

    // munmap check
    if (!(((size_t) curr_node) % PAGE_SIZE) && !(curr_node->size % PAGE_SIZE) && MUNMAP && curr_node->next) {
        // if munmaping start of list update new start
        FREE_LIST = (curr_node == FREE_LIST) ? FREE_LIST : curr_node->next;

        // munmap whole node
        stats.pages_unmapped += (curr_node->size / PAGE_SIZE);
        munmap(curr_node, curr_node->size);

        MUNMAP = !MUNMAP;
    }
}

void
_add_to_free_list(node_t *curr_node, node_t *free_mem, int coalesce) {

    do {
        // if last element of free list, add free node
        if (curr_node->next == NULL && curr_node < free_mem) {

            // insert at end of linked list
            free_mem->next = curr_node->next;
            curr_node->next = free_mem;

            break;
        }

            // if node isnt null, check if its between the two elements
        else if (curr_node < free_mem && free_mem < curr_node->next) {

            // insert into free list
            free_mem->next = curr_node->next;
            curr_node->next = free_mem;

            break;
        }

            // check if node should be placed before current node
        else if (curr_node > free_mem) {

            // insert one previous to end
            free_mem->next = curr_node;
            // replace head if necessary
            FREE_LIST = (curr_node == FREE_LIST) ? free_mem : FREE_LIST;

            break;
        }

    } while ((curr_node = curr_node->next));

    // check if new node allows for coalescing
    if (coalesce) {
        _coalescing_check(FREE_LIST);
    }
}

void *
xmalloc(size_t size) {
    if (!mutex_init_flag) {
        pthread_mutex_init(&free_list_lock, 0);
        mutex_init_flag = 1;
    }

    void *alloced;
    header_t *header_alloced;
    node_t *free_mem;
    node_t *curr_node;
    size_t pages, total_mapped, new_free;

    stats.chunks_allocated += 1;

    // add necessary mapping overhead
    size += sizeof(header_t);

    // If size is > 4088 allocate multiple pages
    if (size >= PAGE_SIZE || !FREE_LIST) {

        // check how many pages are required
        pages = div_up(size, PAGE_SIZE);

        // check if there is any free memory overflow
        // non-zero free memory that could not be added to free list
        // due to mapping overhead
        if (((pages * PAGE_SIZE) - size) && ((pages * PAGE_SIZE) - size) < (sizeof(node_t) + 1))
            pages++;

        // increment stats
        stats.pages_mapped += pages;

        // calculate total amount of bytes mmapped
        total_mapped = (pages * PAGE_SIZE);

        // mmap total pages
        alloced = mmap(NULL,
                       total_mapped,
                       (PROT_READ | PROT_WRITE),
                       (MAP_ANON | MAP_PRIVATE),
                       -1, 0);

        // increment munmap flag
        MUNMAP += 1;

        // set size of new free node
        new_free = (total_mapped - size);

    }
        // check if there is free allocated data on heap
    else {
        pthread_mutex_lock(&free_list_lock);

        curr_node = FREE_LIST;
        node_t *prev_node = FREE_LIST;
        node_t temp;

        // iterate over free list while next isnt null
        do {
            // check if there is enough memory in free node to split it into
            if (curr_node->size >= size + 1) {
                // save values before overwriting them
                new_free = curr_node->size - size;
                temp.next = curr_node->next;

                // overwrite mapping values
                alloced = curr_node;

                // place reduced node back in free list
                if (curr_node == FREE_LIST) {
                    // set values for new free list head
                    curr_node = ((void *) curr_node) + size;
                    curr_node->size = new_free;
                    curr_node->next = temp.next;
                    FREE_LIST = curr_node;

                } else {
                    // set values for new node
                    curr_node += size;
                    curr_node->size = new_free;
                    curr_node->next = temp.next;
                    prev_node->next = curr_node;

                }
                // no new free memory to add
                new_free = 0;

                // break from loop
                break;

            }
                // check if there is exactly enough space
            else if (curr_node->size + sizeof(node_t) == size) {
                // remove current node from free list
                prev_node->next = curr_node->next;

                // reallocated freed memory
                alloced = curr_node;

                // no new free memory
                new_free = 0;

                // break from loop
                break;

            }

                // if no free heap space allocate new page
            else if (curr_node->next == NULL) {

                // initially begin with one page
                pages = 1;

                // check if there will be any overhead
                if (((pages * PAGE_SIZE) - size) && ((pages * PAGE_SIZE) - size) < (sizeof(node_t) + 1))
                    pages++;

                // increment stats
                stats.pages_mapped += pages;

                // allocate new page
                alloced = mmap(NULL,
                               (PAGE_SIZE * pages),
                               (PROT_READ | PROT_WRITE),
                               (MAP_ANON | MAP_PRIVATE),
                               -1, 0);

                // increment unmap flag
                MUNMAP += 1;

                // calculate total amount of bytes mmapped
                total_mapped = (pages * PAGE_SIZE);

                // calculate amount of free memory mapped
                new_free = (total_mapped - size);

                break;
            }
            // save previous node
            prev_node = curr_node;

        } while ((curr_node = curr_node->next));
        pthread_mutex_unlock(&free_list_lock);
    }

    // add header to alloced memory
    header_alloced = (header_t *) alloced;
    header_alloced->size = (size - sizeof(header_t));

    // increment alloced pointer by header size
    alloced += sizeof(header_t);

    // add any unallocated but mapped data to free list
    if (new_free && size < PAGE_SIZE) {

        // find beginning of free memory
        free_mem = (node_t *) (((void *) header_alloced) + size);

        // subtract mapping overhead
        new_free -= sizeof(node_t);
        free_mem->size = new_free;

        // if free list is empty add free memory to it
        if (FREE_LIST == NULL) {
            free_mem->next = NULL;
            FREE_LIST = free_mem;

        }
            // if not first eleement add to free list
        else {
            _add_to_free_list(FREE_LIST, free_mem, 0);

        }
    }
    // return allocated memory pointer
    return alloced;
}

void
xfree(void *item) {
    stats.chunks_freed += 1;

    // Actually free the items
    node_t *free_node;
    header_t *block_header;

    // size of memory block to free
    block_header = ((header_t *) (item - (sizeof(header_t))));

    pthread_mutex_lock(&free_list_lock);

    if ((block_header->size) >= PAGE_SIZE - sizeof(header_t)) {

        // immediately munmap any allocation greater than or equal to page
        stats.pages_unmapped += div_up(block_header->size, PAGE_SIZE - sizeof(header_t));
        munmap(item - sizeof(header_t), block_header->size);

    } else if (!FREE_LIST) {

        // add first element to free list
        FREE_LIST = item - sizeof(header_t);
        FREE_LIST->size = block_header->size - (sizeof(node_t) - sizeof(header_t));
        MUNMAP = (MUNMAP) ? MUNMAP : !(MUNMAP);

    } else {

        // address to beginning of free block
        free_node = item - sizeof(header_t);
        free_node->size = block_header->size - (sizeof(node_t) - sizeof(header_t));
        _add_to_free_list(FREE_LIST, free_node, 1);
        MUNMAP = (MUNMAP) ? MUNMAP : !(MUNMAP);
    }
    pthread_mutex_lock(&free_list_lock);

}

void *
xrealloc(void *item, size_t size) {

    size_t new_free;
    void *new_ptr;
    header_t *block_header;

    // size of memory block to realloc;
    block_header = ((header_t *) (item - (sizeof(header_t))));

    // less memory is required
    if (block_header->size >= size + sizeof(node_t) + 1) {

        node_t *free_mem;

        // new size of memory to add to free list
        new_free = block_header->size - size;

        // set block size to new realloc size
        block_header->size = size;

        // increment pointer value by new size
        free_mem = (node_t *) (item + size);

        // subtract mapping overhead
        new_free -= sizeof(node_t);
        free_mem->size = new_free;

        pthread_mutex_lock(&free_list_lock);
        // if free list is empty add free memory to it
        if (FREE_LIST == NULL) {
            free_mem->next = NULL;
            FREE_LIST = free_mem;

        }
            // if not first eleement add to free list
        else {

            _add_to_free_list(FREE_LIST, free_mem, 0);
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
