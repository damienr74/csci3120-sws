#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

enum {
	NUM_RCBS = 100,
};

/* used for rcb.status */
enum {
	RCB_WAIT,
	RCB_BUSY,
	RCB_DONE,
};

/**
 * structure used to dynamically dispatch scheduler and rcb methods for
 * different implementations
 */
struct interface {
	size_t size;
	void *( *new )( void *this, va_list *args );
	void *( *delete )( void *this );
};

/**
 * structure used to store a request's information in the scheduler
 */
struct rcb {
	const struct interface *interface; /* inherit attributes */
	int seq_num;
	int fd;
	FILE *file;
	long long snt_bytes;
	long long tot_bytes;
	int status;
};

/**
 * structure used to implement many different scheduler implementations
 */
struct scheduler {
	struct interface *interface; /* inherit attributes */
	int ( *compare )( const struct rcb *rcb1, const struct rcb *rcb2 );
	struct rcb **rcbs;
	int count;
	int capacity;
	int quantum;
};

/* scheduler component static dispatch blocks */
extern const void *Rcb;
extern const void *Sjf_scheduler;

/**
 * Picks a scheduler on startup to manage the threadpool.
 */
void scheduler_init( char *sched );

#endif
