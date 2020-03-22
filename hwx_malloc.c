

#include <sys/mman.h>
#include <stdio.h>
#include <assert.h>

#include "xmalloc.h"
#include "list.h"

// If you have a working allocator from the previous HW, use that.
//
// If your previous homework doesn't work, you can use the provided allocator
// taken from the xv6 operating system. It's in xv6_malloc.c
//
// Either way:
//  - Replace xmalloc and xfree below with the working allocator you selected.
//  - Modify the allocator as nessiary to make it thread safe by adding exactly
//    one mutex to protect the free list. This has already been done for the
//    provided xv6 allocator.
//  - Implement the "realloc" function for this allocator.

const size_t PAGE_SIZE = 4096;
static hm_stats stats; // This initializes the stats to 0.

static cell* free_list_head = NULL;

////////////////////////////////////////////////////
////////// Helpers from HW08  //////////////////////

long
free_list_length()
{
    return count_list(free_list_head);
}

cell*
cell_insert_block(cell* to_insert, cell* list_head)
{
    if (list_head == NULL)
    {
        to_insert->rest = NULL;
        return to_insert;
    }

    // inserting before (or at if the size is big enough) the head
    if (to_insert < list_head)
    {
        // Any two adjacent blocks on the free list get coalesced (joined together) into one bigger block.
        // This is the special case where the head needs to be coalesced, by something in front of it
        if ((void*)to_insert + to_insert->curr_size == list_head)
        {
            //at head, in front
            to_insert->curr_size += list_head->curr_size; // add the list to the node
            to_insert->rest = list_head->rest; // replace the head with the node
            return to_insert;
        }

        // replace the head with the node, but keep the head (don't coalesce)
        to_insert->rest = list_head;
        return to_insert;
    }

    // Any two adjacent blocks on the free list get coalesced (joined together) into one bigger block.
    if ((void*)list_head + list_head->curr_size == to_insert)
    {
        //at head, behind
        list_head->curr_size += to_insert->curr_size;
        return cell_insert_block(list_head, list_head->rest); // head is now bigger, need to shift the list down
    }

    // insert after head
    list_head->rest = cell_insert_block(to_insert, list_head->rest);
    return list_head;
}

// See if there’s a big enough block on the free list. If so, select the first one, remove it from the list, and return int
// if not, return null
cell*
xmallocHlp_get_free_block(size_t min_size)
{
    if (free_list_head == NULL)
    {
        return NULL;
    }

    cell* nn = free_list_head;

    //head is big enough to use
    if (free_list_head->curr_size >= min_size)
    {
        free_list_head = free_list_head->rest;
        return nn;
    }

    cell* pp;

    //iterate through the rest of the nodes
    do
    {
        pp = nn; // need to update prev b4 iterating
        nn = nn->rest;

        //stop if end of list OR found block big enough
    } while (nn != NULL && nn->curr_size < min_size);

    if (nn != NULL) // didn't reach end of list
    {
        pp->rest = nn->rest;
    }

    return nn;
}

void
free_list_insert(cell* node)
{
    free_list_head = cell_insert_block(node, free_list_head);
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

/////////////////////////////////////////////////////////
////////// hwx_malloc.c functions  //////////////////////

void*
xmalloc(size_t size)
{
    stats.chunks_allocated += 1;
    size += sizeof(size_t);

    // Use the start of the block to store its size.
    // Return a pointer to the block after the size field.
    void* new_bstart;
    size_t new_bsize;

    // Requests with (B < 1 page = 4096 bytes)
    if (size < PAGE_SIZE)
    {
        //See if there’s a big enough block on the free list. If so, select the first one ...
        cell* node = xmallocHlp_get_free_block(size);

        //  ... and remove it from the list.
        if (node != NULL)
        {
            new_bstart = (void*)node;
            new_bsize = node->curr_size;
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
        if (new_bsize - size > sizeof(cell))
        {
            cell* new_block = (cell*)(new_bstart + size);
            new_block->curr_size = new_bsize - size;
            free_list_insert(new_block);
            new_bsize = size;
        }
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

void
xfree(void* item)
{
    stats.chunks_freed += 1;

    void* bstart = item - sizeof(size_t);
    size_t bsize = *((size_t*)bstart);

    // If the block is < 1 page
    if (bsize < PAGE_SIZE)
    {
        free_list_insert((cell*)bstart); // then stick it on the free list.
    }
    else
    {
        int rv = munmap(bstart, bsize); // then munmap it.
        assert(rv == 0);
        stats.pages_unmapped += bsize / PAGE_SIZE;
    }
}

void*
xrealloc(void* prev, size_t bytes)
{
    // TODO: write realloc
    return 0;
}
