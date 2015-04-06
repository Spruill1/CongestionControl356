
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

//Reliable TCP States will be later transferred to rlib.h
#define RST_CLOSED			0
#define RST_LISTEN			1
#define RST_SYN_SENT		2
#define RST_SYN_RCVD		3
#define RST_ESTABLISHED		4
#define RST_FIN_WAIT_1		5
#define RST_FIN_WAIT_2		6
#define RST_CLOSING			7
#define RST_CLOSE_WAIT		8
#define RST_LAST_ACK		9
#define RST_READ			10
#define RST_WRITE			11

#define MAX_DATA_SIZE		500
struct reliable_state {
	rel_t *next;			/* Linked list for traversing all connections */
	rel_t **prev;
	
	conn_t *c;			/* This is the connection object */
	
	/* Add your own data fields below this */
	clock_t start_time;
	struct config_common *cc;
	struct sockaddr_storage *ss;
	
	int state;
};
rel_t *rel_list;





/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
			const struct config_common *cc)
{
	rel_t *r;
	
	r = xmalloc (sizeof (*r));
	memset (r, 0, sizeof (*r));
	
	if (!c) {
		c = conn_create (r, ss);
		if (!c) {
			free (r);
			return NULL;
		}
	}
	
	r->c = c;
	r->next = rel_list;
	r->prev = &rel_list;
	if (rel_list)
		rel_list->prev = &r->next;
	rel_list = r;
	
	/* Do any other initialization you need here */
	//Initialize timer
	r->start_time = clock();
	
	//Allocate ss and cc, exactly one should be NULL
	if(!ss){
		r->ss = xmalloc(sizeof(struct sockaddr_storage));
		memcpy(&r->ss,ss,sizeof(struct sockaddr_storage));
		cc = NULL;
	} else if(!cc){
		r->cc = xmalloc(sizeof(struct config_common));
		memcpy(&r->cc,cc,sizeof(struct config_common));
		ss = NULL;
	} else
		return NULL;
	
	return r;
}

void
rel_destroy (rel_t *r)
{
	//Manage linked list
	if (r->next)
		r->next->prev = r->prev;
	*r->prev = r->next;
	
	clock_t end_time = clock();
	printf("The time taken to transfer the file was of %ju milliseconds\n",(uintmax_t)(end_time - r->start_time));
	
	conn_destroy (r->c);
	
	/* Free any other allocated memory here */
	//Don't worry about the connection, rlib frees the connection pointer.
	if(r->ss)
		free(r->ss);
	if(r->cc)
		free(r->cc);
	free(r);
	
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
		   const struct sockaddr_storage *ss,
		   packet_t *pkt, size_t len)
{
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	
}


void
rel_read (rel_t *s)
{
	packet_t *packet;
	packet = xmalloc(sizeof(*packet));
	
	int numBytes = conn_input (s->c, packet->data, MAX_DATA_SIZE);
	
	if (numBytes<=0){
		free (packet);
		return;
	}
	//TODO: 3 way habndshake packet with no data first? read 0
	//TODO: Account for states?
	//TODO: EOF?
}

void
rel_output (rel_t *r)
{
}

void
rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */
	
}


