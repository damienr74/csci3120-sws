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
 * Code has been tested, SJF in working condition.
 *
 * TODO change data structure lock to cond_wait instead of mutex
 */

#include "scheduler.h"
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_HTTP_SIZE 8192 /* size of buffer to allocate */

static void *new(const void *_interface, ...);
static void delete( void *this );
void *scheduler_run(void *arg);

static struct scheduler *sched = NULL;
static long long seq_num = 1;
static pthread_mutex_t request_mutex;

void scheduler_init( char *name, int thrd_count ) {
	if ( !name || thrd_count < 1 )
		return;

	if ( !strcmp( "SJF", name ) ) {
		// init Shortest Job First
		if ( !(sched = new(Sjf_scheduler, thrd_count)) ) {
			perror( "Could not init SJF scheduler" );
			abort();
		}

		pthread_mutex_init(&request_mutex, NULL);
		for (int i = 0; i < sched->thrd_count; i++) {
			pthread_create(&sched->threads[i], NULL, &scheduler_run, NULL);
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
	static char *buffer;
	char *req;
	char *brk;
	char *tmp;
	int len;
	struct stat buf;

	if ( fd < 0)
		return;

	if ( !buffer ) {
		buffer = calloc(MAX_HTTP_SIZE, sizeof *buffer);
		if ( !buffer ) {
			perror( "Error while allocating memory" );
			abort();
		}
	}

	if ( read( fd, buffer, MAX_HTTP_SIZE ) <= 0 ) {
		perror( "Error while reading request" );
		abort();
	}

	tmp = strtok_r(buffer, " ", &brk);
	if (tmp && (strcmp("GET", tmp) || !(req = strtok_r(NULL, " ", &brk)))) {
		len = sprintf(buffer, "HTTP/1.1 400 Bad request\n\n");
		write(fd, buffer, len);
		close(fd);
		return;
	}

	if ( (stat(++req, &buf)) ) {
		len = sprintf( buffer, "HTTP/1.1 404 File not found\n\n");
		write(fd, buffer, len);
		close(fd);
		return;
	}

	pthread_mutex_lock(&request_mutex);
	if ( sched->rcb_count >= sched->capacity ) {
		void *p = realloc(sched->rcbs,
				sizeof(void *) * sched->capacity * 2);
		if (!p) {
			perror( "Cannot process request" );
			return;
		}

		sched->rcbs = p;
		sched->capacity *= 2;
	}

	struct rcb *value = new(Rcb, seq_num++, fd, req, buf.st_size);
	if (!value)
		return;

	int index = sched->rcb_count++;
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
	pthread_mutex_unlock(&request_mutex);
}

struct rcb *scheduler_next( void ) {
	struct rcb *value;
	int index, next_index, lchild, rchild;

	if ( sched->rcb_count == 0 )
		return NULL;

	pthread_mutex_lock(&request_mutex);
	value = sched->rcbs[0];

	struct rcb *new_top = sched->rcbs[--sched->rcb_count];

	index = 0;
	int (*cmp)(const struct rcb *rcb1, const struct rcb *rcb2) = sched->compare;

	while (1) {
		lchild = (index << 1) + 1;
		rchild = (index << 1) + 2;

		if (lchild < sched->rcb_count && cmp(sched->rcbs[lchild], new_top) < 0) {
			if (rchild < sched->rcb_count &&
				cmp(sched->rcbs[lchild], sched->rcbs[rchild]) < 0) {
				next_index = lchild;
			} else {
				next_index = rchild;
			}
		} else if (rchild < sched->rcb_count && cmp(sched->rcbs[rchild], new_top) < 0) {
			next_index = rchild;
		} else {
			sched->rcbs[index] = new_top;
			break;
		}

		sched->rcbs[index] = sched->rcbs[next_index];
		index = next_index;
	}

	pthread_mutex_unlock(&request_mutex);
	return value;
}

void *scheduler_run(void *arg) {
	struct rcb *request;

	/* look into pthread_cond_wait to eliminate busy wait */
	for (request = scheduler_next();; request = scheduler_next()) {
		if (!request) {
			continue;
		}

		sched->serve(request);
	}

	return arg;
}


static void *new( const void *this, ... ) {
	const struct interface *interface = this;
	void *instance = calloc( 1, interface->size );

	if (!instance)
		return instance;

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

	this->seq_num = va_arg( *args, long long );
	this->fd = va_arg( *args, int );
	char *filename = va_arg( *args, char * );
	this->tot_bytes = va_arg( *args, off_t );

	this->request = malloc( strlen(filename) + 1 );
	sprintf(this->request, filename);

	this->file = NULL;
	this->snt_bytes = 0;
	this->status = RCB_INIT;

	return this;
}

static void *rcb_delete( void *_this ) {
	struct rcb *this = _this;
	free(this->request);
	fclose(this->file);
	close(this->fd);
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

static void sjf_serve( struct rcb *request ) {
	static char *buffer;
	int len;

	if (!buffer) {
		buffer = calloc(MAX_HTTP_SIZE, sizeof *buffer);
		if (!buffer) {
			perror( "Error allocating memory" );
			abort();
		}
	}

	request->file = fopen(request->request, "r");
	if (!request->file) {
		len = sprintf( buffer, "HTTP/1.1 404 File not found\n\n" );
		write( request->fd, buffer, len );
	} else {
		len = sprintf(buffer, "HTTP/1.1 200 OK\n\n");
		write( request->fd, buffer, len );
	}

	do {
		len = fread(buffer, 1, MAX_HTTP_SIZE, request->file);
		if ( len < 0 ) {
			perror( "Error while reading file" );
			goto stop_sjf_serve;
		}

		request->snt_bytes += len;
		if (len > 0)
			len = write( request->fd, buffer, len );

		if ( len < 0 ) {
			perror( "Error while writing to client" );
			goto stop_sjf_serve;
		}
	} while (request->snt_bytes < request->tot_bytes);

	printf("Request <%lld> completed\n", request->seq_num);

stop_sjf_serve:
	fflush(stdout);
	fflush(stderr);
	delete(request);
}


static void *sjf_new( void *_this, va_list *args ) {
	struct scheduler *this = _this;

	this->compare = sjf_compare;
	this->serve = sjf_serve;
	this->rcbs = calloc( NUM_RCBS, sizeof( void * ) );
	for (int i = 0; i < NUM_RCBS; i++)
		this->rcbs[i] = NULL;

	this->rcb_count = 0;
	this->capacity = NUM_RCBS;
	this->quantum = -1;
	this->thrd_count = va_arg( args, int );
	this->threads = calloc(this->thrd_count, sizeof *this->threads);

	return this;
}

static void *sjf_delete( void *_this ) {
	struct scheduler *this = _this;

	for (int i = 0; i < this->rcb_count; i++)
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
