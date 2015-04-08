
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
#include <stdbool.h>

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

// example uses different server and client states, thoughts? -John

#define ACK_HEADER_SIZE		8
#define PKT_HEADER_SIZE		12
#define MAX_DATA_SIZE		500

/*
     This struct will keep track of packets in our sending/receiving windows
 */
typedef struct window_entry{
	struct window_entry *next;			/* Linked list for traversing all windows */
	struct window_entry *prev;
	
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

	window_entry *window_list;
	
	uint32_t next_seqno;
	
	
	//Sender
	uint32_t lastSeqAcked;
	uint32_t lastSeqWritten;
	uint32_t lastSeqSent;
	
	//Receiver
	uint32_t nextSeqExpected;
	uint32_t lastSeqRead;
	uint32_t lastSeqReceived;

	int state;
};
rel_t *rel_list;



void
smart_add(rel_t *r, packet_t *pkt){
	// TODO: add the packet to the window
}

void
send_ack(rel_t *r){
	// TODO: send an ack for the next seq expected
}



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
	//r->window = (window_entry*)xmalloc(sizeof(window_entry)*cc->window);
	//for(int i=0;i<cc->window;i++){r->window[i].valid=false;}
	r->window_list = NULL;
	
	if (rel_list)
		rel_list->prev = &r->next;
	rel_list = r;
	
	//Sender
	r->lastSeqAcked = 0;
	r->lastSeqWritten = 0;
	r->lastSeqSent = 0;
	
	//Receiver
	r->nextSeqExpected = 0;
	r->lastSeqRead = 0;
	r->lastSeqReceived = 0;

	/* Do any other initialization you need here */
	//Initialize timer
	clock_gettime(CLOCK_MONOTONIC,&r->start_time);

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
	//TODO: free(r->window);

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
	// Check packet formation
	if ((size_t) ntohs(pkt->len) < n){
		return;
	}
	// Check checksum
	uint16_t received_checksum = pkt->cksum;
	memset(&(pkt->cksum),0,sizeof(pkt->cksum));
	if( cksum((void*)pkt, n) != received_checksum){
		return;
	}
	// enforce host byte order
	pkt->len = ntohs (pkt->len);
	pkt->ackno = ntohl (pkt->ackno);
	if (pkt->len > ACK_HEADER_SIZE) pkt->seqno = ntohl(pkt->seqno);

	// Do some stuff with the packets

	// Start by looking for Acks
	if (pkt->len == ACK_HEADER_SIZE){
		process_ack(r,pkt);
	}
	// must be data if it's not corrupted and not an ACK
	else {
		smart_add(r,pkt);
		flush_window(r);
		send_ack(r);
	}
}

/*
  determine the first available window index to place this packet, -1 if none is available
 */
int windowIndex(rel_t *r){
	for(int i = 0; i < r->cc->window; i++){
		//if(!r->window[i].valid) return i;
	}
	return -1;
}

void windowList_enqueue(rel_t *r, window_entry *w){
	window_entry *current;
	if(r->window_list){
		//window_list is null
		r->window_list = w;
		w->next = NULL;
		w->prev = NULL;
		return;
	}
	current = r->window_list;
	//find end
	while(current->next){
		current = current->next;
	}
	current->next = w;
	w->prev = current;
	w->next = NULL;
}

int windowList_dequeue(rel_t *r, window_entry *w){
	if(!r->window_list){
		w = NULL;
		return -1;
	}
	w = r->window_list;
	r->window_list = w->next;
	return 1;
	
}

void
rel_read (rel_t *r)
{
	int bytes_read = 0;
	int window_size = r->lastSeqWritten - r->lastSeqAcked;
	packet_t packet;
	uint32_t packet_size = 0;

	while(1){
		//Check if we can create a new window entry
		if(window_size > r->cc->window || window_size<0){
			printf("ERROR: Window size greater than maximum permitted window size or negative");
			return;
		} else if (window_size == r->cc->window){
			//Window is full!
			return;
		}else if(!(bytes_read = conn_input(r->c, packet.data, MAX_DATA_SIZE)) == 0){
			//Nothing to read
			return;
		} else if(bytes_read<0 && errno==EIO){
				perror("Conn_input failed due to IO error:");
		} else if(!r->state==RST_ESTABLISHED){
			//No established connection yet!
			printf("Connection not yet established!\n");
			return;
		}
		//Valid packet
		window_entry *window = (window_entry *)xmalloc(sizeof(window_entry));
		
		if(bytes_read<0) { // EOF reached
			packet_size = PKT_HEADER_SIZE;
			//EOF packet has no data but has seqno
			packet.seqno = htonl(r->next_seqno); r->next_seqno++;
			packet.len = htons(packet_size);
			packet.ackno=htonl(0);
			packet.cksum=cksum((void*)&packet,PKT_HEADER_SIZE);
			//save packet in window entry
			memcpy(&window->pkt,&packet,sizeof(packet_t));
			window->valid=true;
		}
		else {
			//make a normal packet and add it to the window
			packet_size = PKT_HEADER_SIZE + bytes_read;
			packet.seqno = htonl(r->next_seqno); r->next_seqno++;
			packet.len = htons(packet_size);
			packet.ackno=htonl(0);
			packet.cksum=cksum((void*)&packet,PKT_HEADER_SIZE); //TODO: I believe that the cksum should be over the whole packet, not just the header
			//save packet in window entry
			memcpy(&window->pkt,&packet,sizeof(packet_t));
			window->valid=true;
		}
		
		//enqueue
		windowList_enqueue(r, window);
		//update window parameters
		r->lastSeqWritten = window->pkt.seqno;
		
		//send packet?
		conn_sendpkt(r->c, &window->pkt, packet_size);
		r->lastSeqSent = window->pkt.seqno;
	}


	//I don't think that the packets will actually get sent here...  but maybe?

}

void
rel_output (rel_t *r)
{
	/*
		call conn_output to output data received in UDP packets to STDOUT
		conn_bufspace returns how much space is available for use by conn_output
		
		Acknowledge packets here - if we cannot fit them into the output, don't ack them
		
		library calls this when output has drained, at which point you can call conn_bufspace to
			see how much buffer sapce you have and send out more Acks to get more data from the 
			remote side 
	*/
	
	
}

void
rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */
	//iterate throught the window and if an item is valid & it was transmitted > rel_t->cc->timeout milliseconds ago, then retransmit it.

	rel_t *curr = rel_list;
	while(curr){
		window_entry *curr_win = rel_list->window_list;
		while(curr_win){
			struct timespec currTime; clock_gettime(CLOCK_MONOTONIC,&currTime);
			if(curr_win->valid && currTime.tv_nsec - curr_win->sen.tv_nsec > 
				(curr->cc->timeout*(long)1000000)){
				
				//the packet is still valid (unacked) and has timed-out, retransmit
				clock_gettime(CLOCK_MONOTONIC,&(curr_win->sen)); //udpate the time sent
				conn_sendpkt(curr->c,&(curr_win->pkt),ntohs(curr_win->pkt.len)); //send it
			}
			curr_win = curr_win->next;
		}
		curr = curr->next;
	}

}

