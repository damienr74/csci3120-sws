/**
 * Author: Damien
 *
 * Description: this file holds the scheduler specific methods to init,
 * enqueue, dequeue, and memory manage the incoming requests.
 *
 * The enqueueing and dequeueing mechanisms are scheduler agnostic, every
 * scheduler only has to implement the following dynamic dispatch methods:
 *   - name_cmp, which tells the scheduler_insert and scheduler_next how to
 *	 prioritize the requests.
 *   - name_new, allocate and init the data for the scheduler
 *   - name_delete, deallocate any allocated data and deconstruct the current
 *	 scheduler
 *
 * Code has been tested, SJF in working condition.
 *
 * TODO change data structure lock to cond_wait instead of mutex
 */

#define _POSIX_C_SOURCE 9

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
static pthread_cond_t cond;
static char *buffer;

void scheduler_init( char *name, int thrd_count ) {
	if ( !name || thrd_count < 1 )
		return;

	if ( !strcmp( "SJF", name ) ) {
		// init Shortest Job First
		if ( !(sched = new(Sjf_scheduler)) ) {
			perror( "Could not init SJF scheduler" );
			abort();
		}
	} else if ( !strcmp( "RR", name ) ) {
		// init Round Robin
		if ( !(sched = new(Rr_scheduler)) ) {
			perror( "Could not init SJF scheduler" );
			abort();
		}
	} else if (!strcmp("MLQF", name)) {
		// init Multi-Level Queue with Feedback
		if (!(sched = new(Mlqf_scheduler))) {
			perror("Could not init MLQF scheduler");
			abort();
		}
	} else {
		perror("Scheduler not recognized");
		abort();
	}

	//sleep(15); //****
	pthread_mutex_init(&request_mutex, NULL);
	pthread_cond_init(&cond, NULL);
	pthread_t scheduler;
	if (pthread_create(&scheduler, NULL, &scheduler_run, NULL)) {
		perror( "Could not start scheduler thread" );
		abort();
	}
}

void scheduler_insert( int fd ) {
	static char *buffer;
	char *req = NULL;
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

	struct rcb *request = new(Rcb, seq_num++, fd, req, buf.st_size);
	if (!request)
		return;


	pthread_mutex_lock(&request_mutex);

	sched->insert(request);

	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&request_mutex);
}

struct rcb *scheduler_next( void ) {
	pthread_mutex_lock(&request_mutex);
	while (sched->rcb_count < 1)
		pthread_cond_wait(&cond, &request_mutex);

	struct rcb *request = sched->remove();
	pthread_mutex_unlock(&request_mutex);
	return request;
}

void *scheduler_run(void *arg) {
	struct rcb *request;
	int len;
	if (!buffer) {
		buffer = calloc(MAX_HTTP_SIZE, sizeof *buffer);
		if (!buffer) {
			perror( "Error allocating memory" );
			abort();
		}
	}

	for (request = scheduler_next();; request = scheduler_next()) {
		if (request) {
			if (!request->file) {
				len = sprintf( buffer, "HTTP/1.1 404 File not found\n\n" );
				write( request->fd, buffer, len );
			} else {
				len = sprintf(buffer, "HTTP/1.1 200 OK\n\n");
				write( request->fd, buffer, len );
				// set rcb to started
			}

			sched->serve(request);
		}
	}

	return arg;
}

static void scheduler_serve(struct rcb *request)
{
	if (!request)
		return;

	int len = fread(buffer, 1, MAX_HTTP_SIZE, request->file);
	if ( len < 0 ) {
		perror( "Error while reading file" );
		fflush(stderr);
		delete(request);
		return;
	}

	request->snt_bytes += len;
	if (len > 0)
		len = write( request->fd, buffer, len );

	if ( len < 0 ) {
		perror( "Error while writing to client" );
		fflush(stderr);
		delete(request);
		return;
	}
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
	this = NULL;
}


/*****************************************************************************
 *			Request Control Block static methods & Implementation
 ****************************************************************************/

static void *rcb_new( void *_this, va_list *args ) {
	struct rcb *this = _this;

	this->seq_num = va_arg( *args, long long );
	this->fd = va_arg( *args, int );
	char *filename = va_arg( *args, char * );
	this->tot_bytes = va_arg( *args, off_t );

	this->request = malloc( strlen(filename) + 1 );
	sprintf(this->request, "%s", filename);
	this->file = fopen(this->request, "rb");  //**** rb?

	this->snt_bytes = 0;
	this->status = RCB_8K;
	this->next = NULL;

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
 *			SJF Scheduler static methods & Implementation
 ****************************************************************************/


static int sjf_compare( const struct rcb *rcb1, const struct rcb *rcb2 ) {
	if ( rcb1->tot_bytes < rcb2->tot_bytes )
		return -1;
	else if ( rcb1->tot_bytes == rcb2->tot_bytes )
		return 0;

	return 1;
}

static void sjf_insert(struct rcb *request) {
	if ( sched->rcb_count >= sched->capacity ) {
		void *p = realloc(sched->rcbs,
				sizeof(void *) * sched->capacity * 2);
		if (!p) {
			perror( "Too many requests, out of memory" );
			return;
		}

		sched->rcbs = p;
		sched->capacity *= 2;
	}

	int parent;
	int index = sched->rcb_count++;

	while ( index > 0 ) {
		parent = (index - 1) >> 1;
		if (sched->compare(sched->rcbs[parent], request) < 0) {
			break;
		} else {
			sched->rcbs[index] = sched->rcbs[parent];
			index = parent;
		}
	}

	sched->rcbs[index] = request;
}

static struct rcb *sjf_remove( void ) {
	struct rcb *request;
	int index, next_index, lchild, rchild;

	request = sched->rcbs[0];
	struct rcb *new_top = sched->rcbs[--sched->rcb_count];

	index = 0;
	int (*cmp)(const struct rcb *rcb1, const struct rcb *rcb2) = sched->compare;

	while (1) {
		lchild = (index << 1) + 1;
		rchild = (index << 1) + 2;

		if (lchild < sched->rcb_count &&
				cmp(sched->rcbs[lchild], new_top) < 0) {
			if (rchild < sched->rcb_count &&
				cmp(sched->rcbs[lchild],
					sched->rcbs[rchild]) < 0) {
				next_index = lchild;
			} else {
				next_index = rchild;
			}
		} else if (rchild < sched->rcb_count &&
				cmp(sched->rcbs[rchild], new_top) < 0) {
			next_index = rchild;
		} else {
			sched->rcbs[index] = new_top;
			break;
		}

		sched->rcbs[index] = sched->rcbs[next_index];
		index = next_index;
	}

	return request;
}

static void sjf_serve( struct rcb *request ) {
	do {
		scheduler_serve(request);
	} while (request && request->snt_bytes < request->tot_bytes);

	if (request) {
		printf("Request <%lld> completed\n", request->seq_num);
		fflush(stdout);
		delete(request);
	}
}

static void *sjf_new( void *_this, va_list *args ) {
	struct scheduler *this = _this;

	this->compare = sjf_compare;
	this->insert = sjf_insert;
	this->remove = sjf_remove;
	this->serve = sjf_serve;

	this->rcbs = calloc( NUM_RCBS, sizeof( void * ) );

	for (int i = 0; i < NUM_RCBS; i++)
		this->rcbs[i] = NULL;

	this->rcb_count = 0;
	this->capacity = NUM_RCBS;
	this->quantum = -1;

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
 *           RR Scheduler static methods & Implementation
 *****************************************************************************/

static void linkedlist_insert(struct rcb *request, struct rcb *ll[])
{
	if (!ll[1]) {
			ll[0] = ll[1] = request;
		} else {
			ll[1]->next = request;
			ll[1] = ll[1]->next;
		}

		sched->rcb_count++;
}

static struct rcb* linkedlist_remove(struct rcb* list[])
{
	struct rcb *request = list[0];
		if (request == list[1]) {
			list[0] = list[1] = NULL;
		} else {
			list[0] = request->next;
		}
		sched->rcb_count--;
		request->next = NULL;

		return request;
}

static void rr_insert(struct rcb *request)
{
	linkedlist_insert(request, &sched->rcbs[0]);
}

static struct rcb *rr_remove(void)
{
	return linkedlist_remove(&sched->rcbs[0]);
}

static void rr_serve(struct rcb *request)
{
	scheduler_serve(request);

	if (request) {
		if (request->snt_bytes < request->tot_bytes) {
			sched->insert(request);
		} else {
			printf("Request <%lld> completed\n", request->seq_num);
			delete(request);
		}
		fflush(stdout);
	}
}

void *rr_new(void *_this, va_list *args)
{
	struct scheduler *this = _this;

	this->insert = rr_insert;
	this->remove = rr_remove;
	this->serve = rr_serve;

	this->rcbs = calloc(2, sizeof *this->rcbs);
	this->quantum = MAX_HTTP_SIZE;

	return this;
}

void *rr_delete(void *_this)
{
	struct scheduler *this = _this;

	if (this->rcbs[0]) {
		struct rcb *request;
		while (this->rcbs[0]) {
			request = this->rcbs[0]->next;
			delete(this->rcbs[0]);
			this->rcbs[0] = request;
		}
	}

	free(this->rcbs);

	return this;
}


static const struct interface _Rr_scheduler = {
	sizeof (struct scheduler),
	rr_new,
	rr_delete,
};

const void *Rr_scheduler = &_Rr_scheduler;

/*****************************************************************************
 *				MLQF Scheduler static methods & Implementation
 ****************************************************************************/
static void mlqf_insert(struct rcb *request)
{
// 8kb
	if ( request->status == RCB_8K){
		linkedlist_insert(request, &sched->rcbs[0]);
	}
	else if(request->status == RCB_64K ){
		linkedlist_insert(request, &sched->rcbs[2]);

	}
	else if(request->status == RCB_RR ) {
		linkedlist_insert(request, &sched->rcbs[4]);
	}
}

static struct rcb *mlqf_remove(void)
{
// check 8k
	if (sched->rcbs[0]){
		return linkedlist_remove(&sched->rcbs[0]);
	}
	else if (sched->rcbs[2]){
		return linkedlist_remove(&sched->rcbs[2]);
	}
	else if (sched->rcbs[4]){
		return linkedlist_remove(&sched->rcbs[4]);
	}
	return NULL;
}

static void mlqf_serve(struct rcb *request)
{
	if (request->status == RCB_8K) {

		scheduler_serve(request);

		if (!request)
			return;

		if (request->snt_bytes < request->tot_bytes) {
			request->status++;
			sched->insert(request);
		}
		else {
			delete(request);
			fflush(stdout);
		}

		return;
	}
	if (request->status == RCB_64K) {
		for (int i = 0; i < 8; i++) {
			scheduler_serve(request);
		}

		if (!request)
			return;

		if (request->snt_bytes < request->tot_bytes) {
			request->status++;
			sched->insert(request);
		}
		else {
			delete(request);
			fflush(stdout);
		}
		return;
	}
	if (request->status == RCB_RR) {
		for (int i = 0; i < 8; i++) {
			scheduler_serve(request);
		}

		if (!request)
			return;

		if (request->snt_bytes < request->tot_bytes) {
			sched->insert(request);
		}
		else {
			delete(request);
			fflush(stdout);
		}
	}
}

void *mlqf_new(void *_this, va_list *args)
{
	struct scheduler *this = _this;

	this->insert = mlqf_insert;
	this->remove = mlqf_remove;
	this->serve = mlqf_serve;

	this->rcbs = calloc(6, sizeof *this->rcbs);
	this->quantum = MAX_HTTP_SIZE;

	return this;
}

void *mlqf_delete(void *_this)
{
	struct scheduler *this = _this;

	if (this->rcbs[0]) {
		struct rcb *request;
		while (this->rcbs[0]) {
			request = this->rcbs[0]->next;
			delete(this->rcbs[0]);
			this->rcbs[0] = request;
		}
	}

	free(this->rcbs);

	return this;
}

static const struct interface _Mlqf_scheduler = {
	sizeof (struct scheduler),
	mlqf_new,
	mlqf_delete,
};

const void *Mlqf_scheduler = &_Mlqf_scheduler;
