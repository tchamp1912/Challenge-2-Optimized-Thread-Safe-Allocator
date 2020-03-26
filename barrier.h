// Author: Nat Tuck
// CS3650 starter code

#ifndef BARRIER_H
#define BARRIER_H

#include <pthread.h>

typedef struct barrier {
		pthread_mutex_t mutex;
		pthread_cond_t cond;
    int   frees;
    int   placed;
		int   exit;
} barrier;

void init_barrier(barrier* bb);
void barrier_wait(barrier* bb);
void free_barrier(barrier* bb);


#endif

