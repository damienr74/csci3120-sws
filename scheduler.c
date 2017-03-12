/**
 * Author: Damien
 *
 * Description: this file holds the scheduler specific methods to init,
 * enqueue, dequeue, and memory manage the incomming requests.
 *
 * The enqueueing and dequeueing mechanisms are scheduler agnostic, every
 * scheduler only has to implement the following dynamic dispatch methods:
 *   - name_cmp, which tells the scheduler_insert and scheduler_next how to
 *     prioritize the requests.
 *   - name_new, allocate and init the data for the scheduler
 *   - name_delete, deallocat any allocated data and deconstruct the current
 *     scheduler
 *
 * TODO implement worker method that calls scheduler_next and processes the
 * requests
 *
 * TODO move login in serve_client to scheduler_insert
 *
 * NOTE the scheduler_insert and scheduler_next are not currently in use,
 * after we refactor the serve_client method to use a scheduler method as a
 * thread, we will be able to TEST this code.
 */

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "scheduler.h"

static void *new(const void *_interface, ...);
static void delete( void *this );

static struct scheduler *sched = NULL;

void scheduler_init( char *name ) {
	if ( name )
		return;

	if ( !strcmp( "SJF", name ) ) {
		// init Shortest Job First
		if ( !(sched = new(Sjf_scheduler)) ) {
			perror( "Could not init SJF scheduler" );
			abort();
		}
	} else if ( !strcmp( "RR", name ) ) {
		// init Round Robin
	} else if ( !strcmp( "MLQF", name ) ) {
		// init Multi-Level Queue with Feedback
	} else {
		perror( "Scheduler not recognized" );
		abort();
	}
}

void scheduler_insert( int fd ) {
	if ( fd < 0)
		return;

	if ( sched->count >= sched->capacity ) {
		void *p = realloc(sched->rcbs,
				sizeof(void *) * sched->capacity * 2);
		if (!p) {
			perror( "Cannot process request" );
			return;
		}

		sched->rcbs = p;
		sched->capacity *= 2;
	}

	/* TODO fil from request information dynamically */
	int req_num = 1;
	FILE *file = fopen("empty.txt", "r");
	struct stat sb;

	if ( fstat( fd, &sb ) == -1 ) {
		perror( "Cannot locate file" );
		return;
	}

	struct rcb *value = new(Rcb, req_num, fd, file, 0, sb.st_size, RCB_WAIT);
	int index = sched->count++;
	int parent;

	while ( index > 0 ) {
		parent = (index - 1) >> 1;
		if (sched->compare(sched->rcbs[parent], value) < 0) {
			break;
		} else {
			sched->rcbs[index] = sched->rcbs[parent];
			index = parent;
		}
	}

	sched->rcbs[index] = value;
}

struct rcb *scheduler_next( void ) {
	struct rcb *value;
	int index, next_index, lchild, rchild;

	if ( sched->count == 0 )
		return NULL;

	value = sched->rcbs[0];

	struct rcb *new_top = sched->rcbs[--sched->count];

	index = 0;
	int (*cmp)(const struct rcb *rcb1, const struct rcb *rcb2) = sched->compare;

	while (1) {
		lchild = (index << 1) + 1;
		rchild = (index << 1) + 2;

		if (lchild < sched->count && cmp(sched->rcbs[lchild], new_top) < 0) {
			if (rchild < sched->count &&
				cmp(sched->rcbs[lchild], sched->rcbs[rchild]) < 0) {
				next_index = lchild;
			} else {
				next_index = rchild;
			}
		} else if (rchild < sched->count && cmp(sched->rcbs[rchild], new_top) < 0) {
			next_index = rchild;
		} else {
			sched->rcbs[index] = new_top;
			break;
		}

		sched->rcbs[index] = sched->rcbs[next_index];
		index = next_index;
	}

	return value;
}

static void *new( const void *this, ... ) {
	const struct interface *interface = this;
	void *instance = calloc( 1, interface->size );

	assert( instance );
	*( const struct interface **)instance = interface;

	if ( interface->new ) {
		va_list args;
		va_start( args, this );
		instance = interface->new( instance, &args );
		va_end( args );
	}

	return instance;
}

static void delete( void *this ) {
	const struct interface **interface = this;

	if ( this && *interface && (*interface)->delete )
		this = (*interface)->delete(this);
	free(this);
}


/*****************************************************************************
 *            Request Control Block static methods & Implementation
 ****************************************************************************/

static void *rcb_new( void *_this, va_list *args ) {
	struct rcb *this = _this;

	this->seq_num = va_arg( *args, int );
	this->fd = va_arg( *args, int );
	this->file = va_arg( *args, FILE* );
	this->snt_bytes = va_arg( *args, long long );
	this->tot_bytes = va_arg( *args, long long );
	this->status = va_arg( *args, int );

	return this;
}

static void *rcb_delete( void *_this ) {
	struct rcb *this = _this;

	/* release sequence number ??? */
	fclose(this->file);
	return this;
}

static const struct interface _rcb = {
	sizeof( struct rcb ),
	rcb_new,
	rcb_delete,
};

const void *Rcb = &_rcb;


/*****************************************************************************
 *            SJF Scheduler static methods & Implementation
 ****************************************************************************/

static int sjf_compare( const struct rcb *rcb1, const struct rcb *rcb2 ) {
	if ( rcb1->tot_bytes < rcb2->tot_bytes )
		return -1;
	else if ( rcb1->tot_bytes == rcb2->tot_bytes )
		return 0;

	return 1;
}

static void *sjf_new( void *_this, va_list *args ) {
	struct scheduler *this = _this;

	/* TODO use args to setup number of threads??? */
	this->compare = sjf_compare;
	this->rcbs = calloc( NUM_RCBS, sizeof( void * ) );
	for (int i = 0; i < NUM_RCBS; i++)
		this->rcbs[i] = NULL;

	this->count = 0;
	this->capacity = NUM_RCBS;
	this->quantum = -1;

	return this;
}

static void *sjf_delete( void *_this ) {
	struct scheduler *this = _this;

	for (int i = 0; i < this->count; i++)
		delete(&this->rcbs[i]);

	free(this->rcbs);

	return this;
}

static const struct interface _sjf_scheduler = {
	sizeof( struct scheduler ),
	sjf_new,
	sjf_delete,
};

const void *Sjf_scheduler = &_sjf_scheduler;


/*****************************************************************************
 *            TODO RR Scheduler static methods & Implementation
 *
 * need to implement mlqf_compare, mlqf_new, mlqf_delete, and setup the dynamic
 * dispatch methods
 ****************************************************************************/


/*****************************************************************************
 *            TODO MLQF Scheduler static methods & Implementation
 *
 * need to implement mlqf_compare, mlqf_new, mlqf_delete, and setup the dynamic
 * dispatch methods
 ****************************************************************************/
