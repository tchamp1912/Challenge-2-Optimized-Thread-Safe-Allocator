// Author: Nat Tuck
// CS3650 starter code

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>

#include "barrier.h"

void
init_barrier(barrier* bb)
{
		int rv;
    
		if ((long) bb == -1) {
        perror("malloc(barrier)");
        abort();
    }

    rv = pthread_cond_init(&bb->cond, 0);
    if (rv == -1) {
        perror("cond_init(barrier)");
        abort();
    }

    rv = pthread_mutex_init(&bb->mutex, 0);
    if (rv == -1) {
        perror("mutex_init(mutex)");
        abort();
    }

    bb->frees = 0;
    bb->placed  = 0;
		bb->exit = 0;
}

void
barrier_wait(barrier* bb)
{
		
		// lock the mutex
		pthread_mutex_lock(&bb->mutex);
		
		// wait while all threads are not finished
    while (bb->placed + 1 < bb->frees && !bb->exit) {
				
				// thread wait for condition and mutex
        pthread_cond_wait(&bb->cond, &bb->mutex);
    }
		pthread_mutex_unlock(&bb->mutex);

}

void
free_barrier(barrier* bb)
{
    free(bb);
}

