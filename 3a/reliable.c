
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

#define ACK_HEADER_SIZE		8
#define PKT_HEADER_SIZE		12
#define MAX_DATA_SIZE		500

/*
     This struct will keep track of packets in our sending/receiving windows
*/	
typedef struct window_entry{
	packet_t pkt;
	struct timespec sen;
	bool valid;
}window_entry;

struct reliable_state {
	rel_t *next;			/* Linked list for traversing all connections */
	rel_t **prev;
	
	conn_t *c;			/* This is the connection object */
	
	/* Add your own data fields below this */
	struct timespec start_time;
	struct config_common *cc;
	struct sockaddr_storage *ss;

	window_entry *window; //we need to store sending buffer so that we can retransmit if necessary
	uint32_t next_seqno;
	
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

	r->next_seqno = 1;
	
	//initialize the window
	r->window = (window_entry*)xmalloc(sizeof(window_entry)*cc->window); 
	for(int i=0;i<cc->window;i++){r->window[i].valid=false;}

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
	r->state = RST_LISTEN;
	
	return r;
}

void
rel_destroy (rel_t *r)
{
	//Manage linked list
	if (r->next)
		r->next->prev = r->prev;
	*r->prev = r->next;
	
	struct timespec end_time;
	clock_gettime(CLOCK_MONOTONIC,&end_time);
	printf("The time taken to transfer the file was of %ld milliseconds\n",(end_time.tv_nsec - r->start_time.tv_nsec)/(long)(1000000));
	
	conn_destroy (r->c);
	
	/* Free any other allocated memory here */
	free(r->window);

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

/*
  determine the first available window index to place this packet, -1 if none is available
*/
int windowIndex(rel_t *r){
	for(int i = 0; i < r->cc->window; i++){
		if(!r->window[i].valid) return i;
	}
	return -1;
}

void
rel_read (rel_t *r)
{
	unsigned int bytes_to_read = 0;

	packet_t *pkt = (packet_t*)xmalloc(sizeof(packet_t));
	
	while((bytes_to_read = conn_input(r->c, pkt->data, MAX_DATA_SIZE)) > 0){
		//we can read some number of bytes in and send them in a packet
		int win_index = windowIndex(r);
		if(win_index == 0) return; // must wait for the input
		else if(win_index == -1) { // EOF reached 
			pkt->seqno = htonl(r->next_seqno); r->next_seqno++;
			pkt->len = htons(PKT_HEADER_SIZE);
			pkt->ackno=htonl(0);
			pkt->cksum=cksum((void*)pkt,PKT_HEADER_SIZE);
			memcpy(&(r->window[win_index]),pkt,sizeof(packet_t));

			r->window[win_index].valid=true;
		}
		else {
			//make a normal packet and add it to the window
			pkt->seqno = htonl(r->next_seqno);  r->next_seqno++;
			pkt->len = htons(PKT_HEADER_SIZE+bytes_to_read);
			pkt->ackno=htonl(0);
			pkt->cksum=cksum((void*)pkt,PKT_HEADER_SIZE);
			memcpy(&(r->window[win_index]),pkt,sizeof(packet_t));

			r->window[win_index].valid=true;
		}
	}


	//I don't think that the packets will actually get sent here...  but maybe?



/* - yosh, not sure what you were trying to do here...
	if(r->state==RST_LISTEN){
		//Send SYN
		struct ack_packet *ack;
		ack = xmalloc(sizeof(struct ack_packet));
		ack->ackno = htonl(0); //host to network byte ordering
		ack->len = htons(ACK_HEADER_SIZE); //host to network byte ordering
		
		//Compute checksum
		ack->cksum = cksum((void *)ack, 12); //all packets are 12 bytes - method handles bytes ordering
		
	}

	packet_t *packet;
	packet = xmalloc(sizeof(packet_t));

	int numBytes = conn_input (r->c, packet->data, MAX_DATA_SIZE);
	
	if (numBytes<=0){
		modefree (packet);
		return;
	}
	//TODO: 3 way habndshake packet with no data first? read 0
	//TODO: Account for states?
	//TODO: EOF?
*/
}

void
rel_output (rel_t *r)
{
	
}

void
rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */
	//iterate throught the window and if an item is valid & it was transmitted > rel_t->cc->timeout milliseconds ago, then retransmit it.

	rel_t *curr = rel_list;
	while(curr->next){
		for(int i = 0; i < curr->cc->window; i++){
			struct timespec currTime; clock_gettime(CLOCK_MONOTONIC,&currTime);
			if(curr->window[i].valid && currTime - curr->window[i].)
		}
	}
	
}


