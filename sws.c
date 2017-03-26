/*
 * File: sws.c
 * Author: Alex Brodsky
 * Purpose: This file contains the implementation of a simple web server.
 *          It consists of two functions: main() which contains the main
 *          loop accept client connections, and scheduler_insert(), which
 *          enqueues each client request in the scheduler.
 */

#include "scheduler.h"
#include "network.h"

#include <stdio.h>
#include <stdlib.h>

void usage( void ) {
	printf( "usage: sws <port> <scheduler> <thread_count>\n" );
	printf( "   port: [SJF|RR|MLQF]\n" );
	exit(EXIT_FAILURE);
}


/* This function is where the program starts running.
 *    The function first parses its command line parameters to determine port #
 *    Then, it initializes, the network and enters the main loop.
 *    The main loop waits for a client (1 or more to connect, and then processes
 *    all clients by enqueueing the request in the scheduler queue
 * Parameters:
 *    argc : number of command line parameters (including program name
 *    argv : array of pointers to command line parameters
 * Returns: an integer status code, 0 for success, something else for error.
 */
int main( int argc, char **argv ) {
	int port = -1; /* server port # */
	int fd; /* client file descriptor */

	/* check for and process parameters */
	if( argc < 4 ) {
		printf( "incorrect number of parameters\n" );
		usage();
	}

	if ( sscanf( argv[1], "%d", &port ) < 1 ) {
		printf( "port must be numerical\n" );
		usage();
	}

	network_init( port ); /* init network module */

	long thread_count = strtol(argv[3], NULL, 10);
	scheduler_init( argv[2], thread_count);

	for( ;; ) { /* main loop */
		network_wait(); /* wait for clients */

		for( fd = network_open(); fd >= 0; fd = network_open() ) { /* get clients */
			scheduler_insert( fd ); /* process each client */
		}
	}
}
