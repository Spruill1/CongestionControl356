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
#include <inttypes.h>

#include "rlib.h"

//Reliable TCP States will be later transferred to rlib.h
/*
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
 */

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
	//struct timespec sen;

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

	//uint32_t next_seqno;


	//Sender
	uint32_t lastSeqAcked;
	uint32_t lastSeqWritten;
	uint32_t lastSeqSent;

	//Receiver
	uint32_t nextSeqExpected;
	uint32_t lastSeqRead;
	uint32_t lastSeqReceived;

	//int state;
};
rel_t *rel_list;

//Method Declarations
int windowList_smartAdd(rel_t *r, packet_t *pkt);
//int windowList_dequeue(rel_t *r, window_entry *w);

/*
 * Processes Acks server side, frees up the window depending on the ack.
 * Ignores Acks not in window. Updates lastSeqAcked.
 */
void process_ack(rel_t *r, packet_t* pkt){
	// TODO: redo this method
/*	//check if packet is in window
	uint32_t seqno = pkt->seqno;
	if(seqno < r->lastSeqAcked || r->lastSeqSent<seqno){
		printf("INFO: received ack for %d seqno, not in window %d - %d",pkt->seqno,r->lastSeqAcked,r->lastSeqAcked+r->cc->window);
	}

	//seqno in flush packets
	window_entry *current = r->window_list;
	while(current->pkt->seqno<=seqno && seqno<=r->lastSeqSent){
		windowList_dequeue(r, current);
		free(current);
		current = r->window_list;
	}

	r->lastSeqAcked = seqno;
	//call rel_read
	rel_read(r);*/
}

void
send_ack(rel_t *r){
	// TODO: send an ack for the next seq expected

	/*
        Coming into this method the next sequence number expected should have been updated to
        the first sequence number which we were unable to shift the window past.
	 */

	//make the ack
	packet_t ackPkt;
	ackPkt.len = htons(ACK_HEADER_SIZE);
	ackPkt.ackno = htonl(r->nextSeqExpected);
	ackPkt.cksum = cksum((void*)(&ackPkt),ACK_HEADER_SIZE);

	//send the ack
	conn_sendpkt(r->c, &ackPkt, ACK_HEADER_SIZE);
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

	if (rel_list)
		rel_list->prev = &r->next;
	rel_list = r;

	//r->next_seqno = 1;

	//initialize the window
	r->window_list = xmalloc(sizeof(struct window_entry));
	r->window_list->valid = false;
	window_entry *temp_list1 = r->window_list;
	int i;
	printf("window size is %i\n", cc->window);
	for(i=1;i<cc->window;i++){
		struct window_entry *temp_list2 = xmalloc(sizeof(struct window_entry));
		temp_list2->prev = temp_list1;
		temp_list2->valid = false;
		temp_list2->next = NULL;
		temp_list1->next = temp_list2;
		temp_list1 = temp_list2;
	}

	//Sender
	r->lastSeqAcked = 0;
	r->lastSeqWritten = 0;
	r->lastSeqSent = 0;

	//Receiver
	r->nextSeqExpected = 1;
	r->lastSeqRead = 0;
	r->lastSeqReceived = 0;

	/* Do any other initialization you need here */
	//Initialize timer
	clock_gettime(CLOCK_MONOTONIC,&r->start_time);

	//Allocate ss and cc, exactly one should be NULL
	if(ss){
		r->ss = xmalloc(sizeof(struct sockaddr_storage));
		memcpy(r->ss,ss,sizeof(struct sockaddr_storage));
		cc = NULL;
	} else if(cc){
		r->cc = xmalloc(sizeof(struct config_common));
		memcpy(r->cc,cc,sizeof(struct config_common));
		ss = NULL;
	} else{
		return NULL;
	}

	//r->state = RST_LISTEN;

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
	// free the window
	window_entry *temp_entry = r->window_list;
	while(temp_entry->next != NULL){
		//free(temp_entry->pkt);
		temp_entry = temp_entry->next;
		free(temp_entry->prev);
	}
	free(temp_entry);

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
	printf("I received a packet!\n");
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

	printf("The packet wasn't corrupted!\n");
	// Do some stuff with the packets
	// Start by looking for Acks
	if (pkt->len == ACK_HEADER_SIZE){
		process_ack(r,pkt);
	}
	// must be data if it's not corrupted and not an ACK
	else {
		printf("adding the packet to the window\n");
		int result = windowList_smartAdd(r,pkt);
		printf("processing the output. smart add was: %i\n",result);
		rel_output(r);
		printf("sending an ACK!\n\n");
		send_ack(r);
	}
}

/*void windowList_enqueue(rel_t *r, window_entry *w){
	window_entry *current;
	if(r->window_list == NULL){
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
}*/

/*
 * Adds a packet to the receiving end window.
 * Fills in the blanks in the window.
 * returns -1 when the packet cannot be added
 *          0 when the packet is redundant
 *          1 in success
 */
int windowList_smartAdd(rel_t *r, packet_t *pkt){
	uint32_t seqno = pkt->seqno;
	struct window_entry *w = r->window_list;
	uint32_t window_seqno = r->nextSeqExpected;
	printf("packet seq: %"PRIu32", and nextExpectedSeq:%"PRIu32";\n",seqno,window_seqno);
	while(w != NULL){
		if(seqno == window_seqno){
			if (w->valid == 1){
				return 0; //already there and valid
			}
			printf("pkt->len: %i\n",pkt->len);
			//w->pkt = xmalloc(pkt->len);
			memcpy(&(w->pkt),pkt,pkt->len);
			w->valid = 1;
			printf("w->pkt.len: %i\n",w->pkt.len);
			return 1; //packet added at the right spot
		}
		window_seqno++;
		if (w == NULL){
			printf("how did this happen?\n");
		}
		if (w->next == NULL){
			printf("equally confusing\n");
		}
		w = w->next;
	}
	return -1; //couldn't add

	/*if(r->window_list){
		//Window list is NULL, use nextSeqExpected as reference
		if(seqno<r->nextSeqExpected || seqno>r->nextSeqExpected+r->cc->window){ //TODO: segfaults on r->cc->window
    printf("smartAdd 2a\n");
			//ignore packet that has already been processed or that is too far ahead
			printf("INFO: Package of seqno %d was not added. Already processed or far ahead", seqno);
			return -1;
		} else {
    printf("smartAdd 2a\n");
			//This is a valid seqno, add packet
			w = (window_entry *)xmalloc(sizeof(window_entry));
			memcpy(&w->pkt, pkt, pkt->len);
			clock_gettime(CLOCK_MONOTONIC,&w->sen);
			w->valid = true;
			r->window_list = w;
			w->next = NULL;
			w->prev = NULL;
			return 1;
		}
	}

	//Window_list is not null
	//Use head to check for window size
	uint32_t head_seqno = r->window_list->pkt.seqno;
	if(seqno <= head_seqno || head_seqno+r->cc->window < seqno){
		printf("INFO: Package of seqno %d was not added. Already processed or far ahead", seqno);
		return -1;
	}

	//Valid seqno!

	//Find and close gaps!
	window_entry *current = r->window_list;
	while(current->pkt.seqno < seqno){
		int current_seqno = current->pkt.seqno;

		if(current->next == NULL || current->next->pkt.seqno != (current_seqno+1)){
			//Next window does not exist. Create subsequent window and insert
			w = (window_entry *)xmalloc(sizeof(window_entry));
			w->pkt.seqno = current_seqno + 1;
			w->valid = false;
			w->next = current->next;
			w->prev = current;
			continue;
		}

		//next sequential window must exist.
		window_entry *next = current->next;
		int next_seqno = next->pkt.seqno;

		if(next_seqno == seqno){
			if(next->valid){
				//Next window exists, check if valid, update if necessary
				memcpy(&next->pkt, pkt, pkt->len);
				clock_gettime(CLOCK_MONOTONIC,&next->sen);
				next->valid = true;
				return 1;
			} else
				return 0; //packet was already there!
		}
		current = current->next;
	}
	printf("ERROR: Could not smart add. Not smart enough...\n");
	return -1;*/
}

void slideWindow(rel_t *r){
	struct window_entry *temp = r->window_list;
	// first add another entry to the end of the list
	while(temp->next != NULL){
		temp = temp->next;
	}
	temp->next = xmalloc(sizeof(struct window_entry));
	temp->next->valid = false;
	temp->next->prev = temp;
	temp->next->next = NULL;
	// slide the window and update the head
	r->window_list = r->window_list->next;
	printf("I assume this is where it's breaking?\n");
	if (r->window_list->prev == NULL){
		printf("it's null!\n");
		return;
	}
	// TODO: find out why this is breaking
	//free(r->window_list->prev);
	printf("yuppp\n");
	r->window_list->prev = NULL;
}

/*int windowList_dequeue(rel_t *r, window_entry *w){
	if(!r->window_list){
		w = NULL;
		return -1;
	}
	w = r->window_list;
	r->window_list = w->next;
	return 1;

}*/

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
		}else if((bytes_read = conn_input(r->c, packet.data, MAX_DATA_SIZE)) == 0){
			//Nothing to read
			return;
		} else if(bytes_read<0 && errno==EIO){
			perror("Conn_input failed due to IO error:");
		} /*else if(!r->state==RST_ESTABLISHED){
			//No established connection yet!
			printf("Connection not yet established!\n");
			return;
		}*/
		//Valid packet
		window_entry *window = (window_entry *)xmalloc(sizeof(window_entry));

		if(bytes_read<0) { // EOF reached
			packet_size = PKT_HEADER_SIZE;
			//EOF packet has no data but has seqno
			// TODO: fix this whole fucking method
			//packet.seqno = htonl(r->next_seqno); r->next_seqno++;
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
			// TODO: see above
			//packet.seqno = htonl(r->next_seqno); r->next_seqno++;
			packet.len = htons(packet_size);
			packet.ackno=htonl(0);
			packet.cksum=cksum((void*)&packet,packet_size); //TODO: I believe that the cksum should be over the whole packet, not just the header
			//save packet in window entry
			memcpy(&window->pkt,&packet,sizeof(packet_t));
			window->valid=true;
		}

		//enqueue
		//windowList_enqueue(r, window);
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

	window_entry *traverse = r->window_list;
	while(traverse != NULL) {
		if(!traverse->valid) {break;}
		else if(conn_bufspace(r->c) >= traverse->pkt.len - PKT_HEADER_SIZE){
			//commit the data
			conn_output(r->c,(void*)(traverse->pkt.data),traverse->pkt.len - PKT_HEADER_SIZE);
			traverse=traverse->next;
			slideWindow(r);
			r->nextSeqExpected++; //update the next expected sequence number

			/*window_entry *fetch; //slide the window - delete the newly written packet
			if(windowList_dequeue(r,fetch) > 0){
				free(fetch);
			} else {printf("we done fucked up\n");}*/
		}
	}
}

void
rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */
	//iterate throught the window and if an item is valid & it was transmitted > rel_t->cc->timeout milliseconds ago, then retransmit it.

	// TODO: fix this method, possibly with a different window
	/*rel_t *curr = rel_list;
	while(curr){
		window_entry *curr_win = rel_list->window_list;
		while(curr_win){
			struct timespec currTime; clock_gettime(CLOCK_MONOTONIC,&currTime);
			if(curr_win->valid && currTime.tv_nsec - curr_win->sen.tv_nsec >
		(curr->cc->timeout*(long)1000000)){

				//the packet is still valid (unacked) and has timed-out, retransmit
				clock_gettime(CLOCK_MONOTONIC,&(curr_win->sen)); //udpate the time sent
				conn_sendpkt(curr->c,&(curr_win->pkt),ntohs(curr_win->pkt->len)); //send it
			}
			curr_win = curr_win->next;
		}
		curr = curr->next;
	}*/

}

