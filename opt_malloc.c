#include <pthread.h>
#include <sys/sysinfo.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>

#include "barrier.h"
#include "opt_malloc.h"


// --------- PREPROCESSOR DEFINITIONS ------------------------------

// size of a linux page
#define PAGE_SIZE 4096

#define ARENA_MULT 2

#define MAINTANENCE_THREADS 1

#define ARENA_MAX 32

// --------- CONSTRUCTOR/DESTRUCTOR PROTOTYPES ----------------------


// constructor attribute... initializes all mutexes on startup,
// no thread safe properties
void initialize_colliseum (void) __attribute__ ((constructor));

// destructor attribute... frees all buckets on when program
// terminates
void free_colliseum (void) __attribute__ ((destructor));


// --------- TYPEDEFS -----------------------------------------------


// every freed piece of memory will be added
// to a linked list of nodes
typedef struct node_t {
	size_t size;
	struct node_t *next;
} node_t;

// every allocated chunk of memory will have 
// a size of the allocation preceding it
typedef struct header_t {
	size_t size;
} header_t;

// the free list will be split up into 2x n-cores
// arenas that will be in contention between all of the
// threads
typedef struct arena_t {
	struct node_t *first;
	struct node_t *last;
	size_t size;
} arena_t;

// the colliseum will contain all of the arenas and the
// mutexes that will manage the memory access of different
// threads
typedef struct colliseum_t {
	struct arena_t first[ARENA_MAX];
  pthread_mutex_t mut[ARENA_MAX];
	int arenas;
	size_t nodes;
} colliseum_t;

// --------- TOOLS ------------------------------------------------

static
size_t
div_up(size_t xx, size_t yy)
{
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx) {
        return zz;
    }
    else {
        return zz + 1;
    }
}


// --------- THREAD LOCALS ------------------------------------------


// --------- GLOBALS -----------------------------------------------


// bit flag on whether or not to unmap a an allocated page
static int g_Munmap = 1;

// the free list global 
static pthread_mutex_t g_Free_Qutex;
static arena_t g_Free_Queue;
static colliseum_t g_Colliseum;

// the maintanence thread id and barrier
static pthread_t g_Maintanence_t;
static barrier g_Maintanence_bb;

// ----------- MAINTANENCE THREAD WORKER ---------------------------

node_t* merge(node_t *n_1, node_t *n_2, node_t *l_1, node_t *l_2) 
{ 
    if (n_1 == l_1->next) {
        return n_2; 
		}
    if (n_2 == l_2->next) 
        return n_1; 
  
    // start with the linked list 
    // whose head data is the least 
    if (n_1 < n_2) { 
        n_1->next = merge(n_1->next, n_2, l_1, l_2); 
        return n_1; 
    } 
    else { 
        n_2->next = merge(n_1, n_2->next, l_1, l_2); 
        return n_2; 
    } 
} 


void 
merge_two_arenas(arena_t *one, arena_t *two)
{
		// coalesce both into one arena
		one->first = (one->first < two->first)?merge(one->first, two->first, one->last, two->last):NULL;
		two->first = (one->first < two->first)?NULL:merge(one->first, two->first, one->last, two->last);

		if (one->first < two->first) {
			one->last = (one->last < two->last)?two->last:one->last;
			two->last = NULL;
		}
		else {
			two->last = (one->last < two->last)?two->last:one->last;
			one->last = NULL;
		}

		one->size = (one->first == NULL)?0:(size_t)(one->size + two->size);
		two->size = (two->first == NULL)?0:(size_t)(one->size + two->size);

}

void 
bubble_sort_arena(arena_t *curr_arena) 
{ 
    int swapped; 
		node_t *curr_node;
		node_t *left_node = curr_arena->last->next;
		node_t *temp;
    
		do
    { 
        swapped = 0; 
        curr_node = curr_arena->first; 
				
  
        while (curr_node->next != left_node) 
        { 
            if (curr_node > curr_node->next) 
            {  
								// swap link list values
								temp = curr_node->next;
								curr_node->next = temp->next;
								temp->next = curr_node;

								// if first and last arena pointers change
                curr_arena->first = (curr_node == curr_arena->first)?temp:curr_arena->first;
                curr_arena->last = (curr_node == curr_arena->last)?curr_node:curr_arena->last;

                swapped = 1; 
            } 
            curr_node = curr_node->next; 
        } 
        left_node = curr_node; 
    } 
    while (swapped); 
} 


void
coalescing_check(node_t *curr_node, node_t *end_node)
{
			
			if (curr_node == NULL || curr_node->next == end_node)
				return;

			// coalescing check
			if ((((size_t) curr_node) + curr_node->size + sizeof(node_t)) == ((size_t) curr_node->next) && curr_node->next) {

					// increment node size by next node
					curr_node->size += curr_node->next->size + sizeof(node_t);

					// change node next
					curr_node->next = curr_node->next->next;

			}

			// recursively call on the 
			coalescing_check(curr_node->next, end_node);
}

void 
munmap_check(arena_t *curr_arena, node_t *prev_node, node_t *curr_node, node_t *end_node)
{
			if (curr_node == NULL || curr_node->next == end_node)
				return;

			// munmap check
			if (!(((size_t) curr_node) % PAGE_SIZE) && !(curr_node->size % PAGE_SIZE) && g_Munmap) {
				// if munmaping start of list update new start
				curr_arena->first = (prev_node)?curr_arena->first:curr_node->next;

				curr_arena->last = (curr_node == curr_arena->last)?prev_node:curr_arena->last;

				// munmap whole node
				munmap(curr_node, curr_node->size);

				g_Munmap = !g_Munmap;
			}
			// recursively call on the 
			munmap_check(curr_arena, curr_node, curr_node->next, end_node);
}

// returns non-zero integer when the worker should continue to wait
void
first_to_arena(arena_t *curr_arena) 
{
		
		// check if first value in arena
		if (curr_arena->first == NULL) {
			curr_arena->first = g_Free_Queue.first;
			curr_arena->size = g_Free_Queue.first->size;
			

		}
		// add it to the end of arena
		else {
			curr_arena->last = g_Free_Queue.first;
			curr_arena->size += g_Free_Queue.first->size;

		}

		// reduce Queue size
		g_Free_Queue.size -= g_Free_Queue.first->size;

		// change head of queue to next 
		g_Free_Queue.first = g_Free_Queue.first->next;

		// increment barrier values
		g_Maintanence_bb.placed++;


}

void
queue_to_arena()
{

			// check which arena to use
			arena_t *curr_arena = NULL;
			int lock_status;
			int ii = 0;
			while (g_Free_Queue.size > sizeof(int)) {
					
				// continue again
				ii %= g_Colliseum.arenas;

				// try lock the mutex of each arena
				lock_status = pthread_mutex_trylock(g_Colliseum.mut + ii);

				// if lock succeeds use that arena
				if (lock_status == 0){

					curr_arena = g_Colliseum.first + ii;
						
					// run maintenance routines on arena
					first_to_arena(curr_arena); 

					// unlock thread
					pthread_mutex_unlock(g_Colliseum.mut + ii);			

				}	
			}
				

}

void
sort_arenas()
{

		int f_lock_st, s_lock_st;
		arena_t *f_curr_arena, *s_curr_arena;

		for (int ii = 0; ii < g_Colliseum.arenas; ii++) {

				// try lock the mutex of each arena
				f_lock_st = pthread_mutex_trylock(g_Colliseum.mut + ii);

				// if lock succeeds use that arena
				if (f_lock_st == 0){
					
					f_curr_arena = g_Colliseum.first + ii;

					// sort current arena
					bubble_sort_arena(f_curr_arena);

					for (int jj = 0; jj < g_Colliseum.arenas; jj++) {
						// try lock the mutex of each arena
						s_lock_st = pthread_mutex_trylock(g_Colliseum.mut + jj);

						// if lock succeeds use that arena
						if (s_lock_st == 0){
					
							s_curr_arena = g_Colliseum.first + jj;

							// sort second arena
							bubble_sort_arena(s_curr_arena);

							// merge the two arenas
							merge_two_arenas(f_curr_arena, s_curr_arena);
							
							pthread_mutex_unlock(g_Colliseum.mut + jj);

							break;
			
						}
					}
					// unlock first arena
					pthread_mutex_unlock(g_Colliseum.mut + ii);

				}

		}

}


void 
coalesce_arenas()
{
		int f_lock_st;
		arena_t *f_curr_arena;
		
		for (int ii = 0; ii < g_Colliseum.arenas; ii++) {

				// try lock the mutex of each arena
				f_lock_st = pthread_mutex_trylock(g_Colliseum.mut + ii);

				// if lock succeeds use that arena
				if (f_lock_st == 0){
					
					f_curr_arena = g_Colliseum.first + ii;

					coalescing_check(f_curr_arena->first, f_curr_arena->last);

					if (f_curr_arena->size > PAGE_SIZE)
						munmap_check(f_curr_arena, NULL, f_curr_arena->first, f_curr_arena->last);
				
				}
		}

}


void*
maintanence_worker(void *arg){
	
		while (1){
				// wait until free queue is big enough to work with
				barrier_wait(&g_Maintanence_bb);

				queue_to_arena();

				sort_arenas();

				coalesce_arenas();
		}

}

// --------- CONSTRUCTOR/DESTRUCTOR FUNCTIONS -----------------------


// called when program starts
void 
initialize_colliseum(void) 
{

		// the number of arenas in a colliseum some multiple of the 
		// number of proc
		g_Colliseum.arenas = (get_nprocs() * ARENA_MULT);

		for (int ii = 0; ii < g_Colliseum.arenas; ii++) {

			// intialize the node_t pointers to NULL
			g_Colliseum.first[ii].first = NULL;
			g_Colliseum.first[ii].last = NULL;
			g_Colliseum.first[ii].size = 0;

			// intialize mutexes to unlocked
			pthread_mutex_init(&g_Colliseum.mut[ii], 0);
		}
		// initialize free list and queue mutexes
		pthread_mutex_init(&g_Free_Qutex, 0);
		
		// intialize barrier
		init_barrier(&g_Maintanence_bb);

		// start maintanence thread	
		pthread_create(&g_Maintanence_t, NULL, maintanence_worker, NULL);
}


// called when program exits
void 
free_colliseum(void) 
{

	g_Maintanence_bb.exit = 1;
	pthread_cond_broadcast(&g_Maintanence_bb.cond);
	// munmap the whole free list
	
	
	// join maintanence thread
	pthread_join(g_Maintanence_t, 0);

}




void 
add_to_free_queue(node_t *free_mem) {


		// add new memory to free queue arena
		// if its the first node in queue 
		if (g_Free_Queue.first == NULL) {
			// lock free queue
			pthread_mutex_lock(&g_Free_Qutex);
			
			// add to free queue
			g_Free_Queue.first = free_mem;

			// unlock the free queue mutex
			pthread_mutex_unlock(&g_Free_Qutex);

		}
		// if its second node in queue
		else if (g_Free_Queue.last == NULL) {
			
			// add to free queue
			g_Free_Queue.last = free_mem;

		}
		// else change pointer to last 
		else {
			// point old last at new free mem
			g_Free_Queue.last->next = free_mem;

			// point last node of arena at new free mem
			g_Free_Queue.last = free_mem;

		}
		// increment size of arena
		g_Free_Queue.size += free_mem->size;

		// increment barrier value
		g_Maintanence_bb.frees++;

		// broadcast to the maintanence worker
		pthread_cond_broadcast(&g_Maintanence_bb.cond);

}


void*
xmalloc(size_t size)
{
		void *alloced;
		header_t *header_alloced;
		node_t *free_mem;
		size_t pages, total_mapped, new_free;

		// check which arena to use
		arena_t *curr_arena = NULL;
		int lock_status;

		// iterate over arenas
		int ii = 0;
		for (; ii < g_Colliseum.arenas; ii++) {

			// try lock the mutex of each arena
			lock_status = pthread_mutex_trylock(g_Colliseum.mut + ii);

			// if lock succeeds use that arena
			if (lock_status == 0 && (g_Colliseum.first[ii].size) > size) {
				
				// get arena pointer value
				curr_arena = g_Colliseum.first + ii;

				// break if successful
				break;

			}

			// relock mutex if arena isnt big enough
			else if (lock_status == 0) {
				pthread_mutex_unlock(g_Colliseum.mut + ii);

			}

		}

		// add necessary mapping overhead
    size += sizeof(header_t);
		
		// If size is > 4088 allocate multiple pages
		if (size >= PAGE_SIZE || !curr_arena) {
			// unlock mutex
			pthread_mutex_unlock(g_Colliseum.mut + ii);

			// check how many pages are required 
			pages = div_up(size, PAGE_SIZE);

			// check if there is any free memory overflow
			// non-zero free memory that could not be added to free list
			// due to mapping overhead
			if (((pages * PAGE_SIZE) - size) && ((pages * PAGE_SIZE) - size) < (sizeof(node_t) + 1))
				pages++;

			// calculate total amount of bytes mmapped
			total_mapped = (pages * PAGE_SIZE);
			
			// mmap total pages
			alloced = mmap(NULL, 
										 total_mapped,
										 (PROT_READ | PROT_WRITE),
										 (MAP_ANON | MAP_PRIVATE),
										 -1, 0);

			// increment munmap flag
			g_Munmap += 1;

			// set size of new free node
			new_free = (total_mapped - size);

		}	
		// check if there is free allocated data on heap
		else {
				node_t *curr_node = curr_arena->first;
				node_t *prev_node = curr_arena->first;
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

						// place reduced node back in free arena
						if (curr_node == curr_arena->first || curr_node == curr_arena->last) {
							// set values for new free list head
							curr_node = ((void*) curr_node) + size;
							curr_node->size = new_free;
							curr_node->next = temp.next;
							curr_arena->first = (curr_node == curr_arena->first)?curr_node:curr_arena->first;
							curr_arena->last = (curr_node == curr_arena->last)?curr_node:curr_arena->last;

						}
						else {
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
						prev_node->next = (curr_node == curr_arena->first)?NULL:curr_node->next;

						// reallocated freed memory
						alloced = curr_node;

						// check if the nodes were the first or last and replace if so
						curr_arena->first = (curr_node == curr_arena->first)?curr_node->next:curr_arena->first;
						curr_arena->last = (curr_node == curr_arena->last)?prev_node:curr_arena->last;

						// no new free memory
						new_free = 0;

						// break from loop
						break;

					}

					// if no free heap space allocate new page
					else if (curr_node->next == NULL || curr_node == curr_arena->last){

						// initially begin with one page
						pages = 1;

						// check if there will be any overhead
						if (((pages * PAGE_SIZE) - size) && ((pages * PAGE_SIZE) - size) < (sizeof(node_t) + 1))
							pages++;
						
						// allocate new page
						alloced = mmap(NULL, 
											 		(PAGE_SIZE * pages),
											 		(PROT_READ | PROT_WRITE),
											 		(MAP_ANON | MAP_PRIVATE),
											 		-1, 0);

						// increment unmap flag
						g_Munmap += 1;
						
						// calculate total amount of bytes mmapped
						total_mapped = (pages * PAGE_SIZE);
			
						// calculate amount of free memory mapped
						new_free = (total_mapped - size);
	
						break;
					}	

					// save previous node
					prev_node = curr_node;

					// move to next node
					curr_node = curr_node->next;

				} while ((curr_node != NULL) || (curr_node != curr_arena->last));
			}

		// add header to alloced memory
		header_alloced = (header_t*) alloced;
		header_alloced->size = (size - sizeof(header_t));

		// increment alloced pointer by header size
		alloced += sizeof(header_t);

		// add any unallocated but mapped data to free list
		if (new_free && size < PAGE_SIZE) {

			// find beginning of free memory
			free_mem = (node_t*) (((void*) header_alloced) + size);

			// subtract mapping overhead
			new_free -= sizeof(node_t);

			// set node_t values
			free_mem->size = new_free;
			free_mem->next = NULL;

			// add the free memory to free queue to be placed
			add_to_free_queue(free_mem);

		}
		// return allocated memory pointer
		return alloced;
}

void
xfree(void* item)
{

    // Actually free the items
		node_t *free_node;
		header_t *block_header;

		// size of memory block to free
		block_header = ((header_t*) (item - (sizeof(header_t))));

		if ((block_header->size) >= PAGE_SIZE - sizeof(header_t)) {

			// immediately munmap any allocation greater than or equal to page
			munmap(item - sizeof(header_t), block_header->size);

		}

		else {

			// address to beginning of free block
			free_node = item - sizeof(header_t);
			free_node->size = block_header->size - (sizeof(node_t) - sizeof(header_t));
			add_to_free_queue(free_node);	
			g_Munmap = (g_Munmap)?g_Munmap:!(g_Munmap);

		}	
}

void*
xrealloc(void *item, size_t size)
{

		size_t new_free;
		void *new_ptr;
		header_t *block_header;

		// size of memory block to realloc;
		block_header = ((header_t*) (item - (sizeof(header_t))));

		// less memory is required
		if (block_header->size >= size + sizeof(node_t) + 1) {

			node_t *free_mem;

			// new size of memory to add to free list
			new_free = block_header->size - size;

			// set block size to new realloc size
			block_header->size = size;

			// increment pointer value by new size
			free_mem = (node_t*) (item + size);

			// subtract mapping overhead
			new_free -= sizeof(node_t);
			free_mem->size = new_free;

			add_to_free_queue(free_mem);

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
