/*
 * File: ReliableSocket.cpp
 * 
 * Reliable data transport (RDT) library implementation.
 *
 * Author(s): Justin Cavalli, Chadmond Wu
 *
 */

// C++ library includes
#include <iostream>

//OS specific includes
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cstring>

#include "ReliableSocket.h"
#include "rdt_time.h"

using std::cerr;

/*
* NOTE: Function header comments shouldn't go in this file: they should be put
* in the ReliableSocket header file
*/

ReliableSocket::ReliableSocket() {
	this->sequence_number = 0;
	this->expected_sequence_number = 0;
	this->estimated_rtt = 100;
	this->dev_rtt = 10;
	this->current_rtt = 0;

	this->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (this->sock_fd < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	this->state = INIT;
}

void ReliableSocket::accept_connection(int port_num) {
	if (this->state != INIT) {
		cerr << "cannot call accept on used socket\n";
		exit(EXIT_FAILURE);
	}

	// Bind specified port num using our local IPv4 address.
	// This allows remote hosts to connect to a specific port.
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port_num);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(this->sock_fd, (struct sockaddr*)&addr, sizeof(addr))) {
		perror("bind");
	}

	// Wait for a segment to come from a remote host
	char segment[MAX_SEG_SIZE];
	memset(segment, 0, MAX_SEG_SIZE);

	struct sockaddr_in fromaddr;
	unsigned int addrlen = sizeof(fromaddr);
	int recv_count = recvfrom(this->sock_fd, segment, MAX_SEG_SIZE, 0, (struct sockaddr*)&fromaddr, &addrlen);
	
	if (recv_count < 0) {
		perror("accept recvfrom");
		exit(EXIT_FAILURE);
	}

	/*
	 * UDP isn't connection-oriented, but calling connect here allows us to
	 * remember the remote host (stored in fromaddr).
	 * This means we can then use send and recv instead of the more complex
	 * sendto and recvfrom.
	 */
	if (connect(this->sock_fd, (struct sockaddr*)&fromaddr, addrlen)) {
		perror("accept connect");
		exit(EXIT_FAILURE);
	}

	// Check that segment was the right type of message, namely a RDT_SYN
	// message to indicate that the remote host wants to start a new
	// connection with us.
	RDTHeader* hdr = (RDTHeader*)segment;
	if (hdr->type != RDT_SYN) {
		cerr << "ERROR: Didn't get the expected RDT_SYN type.\n";
		cerr << "Connection was not Established\n";
		exit(EXIT_FAILURE);
	}

	// Send an RDT_SYNACK in response to the RDT_SYN
	char send_seg[MAX_SEG_SIZE] = {0};
	char recv_seg[MAX_SEG_SIZE];

	hdr = (RDTHeader*)send_seg;
	hdr->ack_number = htonl(0);
	hdr->sequence_number = htonl(0);
	hdr->type = RDT_SYNACK;

	do
	{
		set_timeout_length(this->estimated_rtt + (4 * this->dev_rtt));
		this->reliable_send(send_seg, sizeof(RDTHeader), recv_seg);

		hdr = (RDTHeader*)recv_seg;
		
		// Check we get an ACK in return
		if (hdr->type == RDT_ACK) {
			break;
		} 
		// Assume the ACK was dropped and data is now being sent
		else if (hdr->type == RDT_DATA) {
			break;
		}
		else {
			// Didn't get an RDT_ACK or RDT_DATA
			continue;
		}
	} while (true);

	cerr << "Connection ESTABLISHED\n";
	this->state = ESTABLISHED;
}


void ReliableSocket::connect_to_remote(char *hostname, int port_num) {
	if (this->state != INIT) {
		cerr << "Cannot call connect_to_remote on used socket\n";
		return;
	}

	// Set up IPv4 address info with given hostname and port number
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;	// use IPv4
	addr.sin_addr.s_addr = inet_addr(hostname);
	addr.sin_port = htons(port_num);

	/*
	 * UDP isn't connection oriented, but calling connect here allows us to
	 * remember the remote host (stored in fromaddr).
	 * This means we can then use send and recv instead of the more complex
	 * sendto and recvfrom.
	 */
	if (connect(this->sock_fd, (struct sockaddr*)&addr, sizeof(addr))) {
		perror("connect");
	}

		// Send an RDT_SYN message to remote host to initiate an RDT connection.
		char send_seg[MAX_SEG_SIZE] = {0};
		char recv_seg[MAX_SEG_SIZE];

		RDTHeader* hdr = (RDTHeader*)send_seg;
		hdr->ack_number = htonl(0);
		hdr->sequence_number = htonl(0);
		hdr->type = RDT_SYN;

		this->set_timeout_length(this->estimated_rtt + (4 * this->dev_rtt));
		this->reliable_send(send_seg, sizeof(RDTHeader), recv_seg);
		
		// Expecting a SYNACK in return for the RDT_SYN
		memset(hdr, 0, sizeof(RDTHeader));
		hdr = (RDTHeader*)recv_seg;
		if (hdr->type != RDT_SYNACK) {
			// Response was not an RDT_SYNACK type
			perror("Message was not a SYNACK");
		}


		// Send a final ACK for the three way handshake
		memset(send_seg, 0, sizeof(RDTHeader));
		hdr = (RDTHeader*)send_seg;
		hdr->ack_number = htonl(0);
		hdr->sequence_number = htonl(0);
		hdr->type = RDT_ACK;
		this->timeout_send(send_seg);

		this->state = ESTABLISHED;
		cerr << "INFO: Connection ESTABLISHED\n";
}

void ReliableSocket::reliable_send(char send_seg[MAX_SEG_SIZE], int send_seg_size, char recv_seg[MAX_SEG_SIZE]) {
	int time_sent;
	this->set_timeout_length(this->estimated_rtt + (4 * this->dev_rtt));
	// Keeps track if the previous send timed out
	bool previous_timeout = false;
	// Stores the previous timeout time. So it can be doubled in the case of a
	// timeout
	uint32_t doubled_timeout;
	do {
			// Get time of send to calculate current_rtt
			time_sent = current_msec();
			// Send the send_seg
			if (send(this->sock_fd, send_seg, send_seg_size, 0) < 0) {
				perror("reliable_send send");
			}
			// Get ready to receive the segment
			memset(recv_seg, 0, MAX_SEG_SIZE);
			int bytes_received = recv(this->sock_fd, recv_seg, MAX_SEG_SIZE, 0);
			if (bytes_received < 0) {
				if (errno == EAGAIN) {
					// set the timeout length to double whatever it was previously
					cerr << "Timeout Occurred. Doubling the length.\n";
					if (previous_timeout) {
						doubled_timeout *= 2;
						this->set_timeout_length(doubled_timeout);
					}
					else {
						doubled_timeout = (2 * (this->estimated_rtt + 4 * this->dev_rtt));
						this->set_timeout_length(doubled_timeout);
					}
					previous_timeout = true;
					continue;
				}
				else {
					// Some other error than timeouts
					perror("ACK not received");
					exit(EXIT_FAILURE);
				}
			}

			this->current_rtt = current_msec() - time_sent;
			break;
	} while (true); 

		// Update the timeout length using the new current_rtt
		this->set_estimated_rtt();
}

void ReliableSocket::timeout_send(char send_seg[]) {
	char recv_seg[MAX_SEG_SIZE];

	do {
			if (send(this->sock_fd, send_seg, sizeof(RDTHeader), 0) < 0) {
				perror("timeout_send send");
			}

			memset(recv_seg, 0, MAX_SEG_SIZE);
			this->set_timeout_length(this->estimated_rtt + (this->dev_rtt * 4));
			if (recv(this->sock_fd, recv_seg, MAX_SEG_SIZE, 0) < 0) {
				if (errno == EAGAIN) {
					// The segment timed out as expected
					break;
				}
				else {
					perror("timeout_send recv");
					exit(EXIT_FAILURE);
				}
			} else {
				// Got a packet in return so continue the loop
				continue;
			}
	} while (true);

}

// You should not modify this function in any way.
uint32_t ReliableSocket::get_estimated_rtt() {
	return this->estimated_rtt;
}

void ReliableSocket::set_estimated_rtt() {
	// calculate the estimated_rtt
	this->estimated_rtt *= (1 - 0.125);
	this->estimated_rtt += this->current_rtt * 0.125;
	this->dev_rtt *= (1 - 0.25);
	// Find the difference (can't be negative)
	int abs_dev = this->current_rtt - this->estimated_rtt;
	if (abs_dev < 0) {
		abs_dev *= -1;
	}
	this->dev_rtt += (abs_dev * 0.25);
	// Update the timeout length
	this->set_timeout_length(this->estimated_rtt + 4 * this->dev_rtt);
}

// You shouldn't need to modify this function in any way.
void ReliableSocket::set_timeout_length(uint32_t timeout_length_ms) {
	cerr << "INFO: Setting timeout to " << timeout_length_ms << " ms\n";
	struct timeval timeout;
	msec_to_timeval(timeout_length_ms, &timeout);

	if (setsockopt(this->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,sizeof(struct timeval)) < 0) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
}

void ReliableSocket::send_data(const void *data, int length) {
	if (this->state != ESTABLISHED) {
		cerr << "INFO: Cannot send: Connection not established.\n";
		return;
	}

	// Create the segment, which contains a header followed by the data.
	char send_seg[MAX_SEG_SIZE] = {0};
	char recv_seg[MAX_SEG_SIZE];

	// Fill in the header
	RDTHeader *hdr = (RDTHeader*)send_seg;
	hdr->sequence_number = htonl(this->sequence_number);
	hdr->ack_number = htonl(0);
	hdr->type = RDT_DATA;

	// Copy the user-supplied data to the spot right past the
	// header (i.e. hdr+1).
	memcpy(hdr + 1, data, length);

	do {
			// Send the data
			memset(recv_seg, 0, MAX_SEG_SIZE);
			reliable_send(send_seg, sizeof(RDTHeader) + length, recv_seg);

			// Check the type of message that was received
			hdr = (RDTHeader*)recv_seg;
			if (hdr->type == RDT_ACK) {
				if (this->sequence_number == ntohl(hdr->ack_number)) {
					// Expected ACK was received
					break;
				} else {
	
						// Out of Order ACK was received
						continue;
					}
			} else {
					// An ACK was not received so continue loop;
					continue;
			}
	} while (true);
	
	// Packet successfully sent so increase the seqnum
	this->sequence_number++;
}


int ReliableSocket::receive_data(char buffer[MAX_DATA_SIZE]) {
	// We don't want the reciever timing out when receiving data
	this->set_timeout_length(0);
	if (this->state != ESTABLISHED) {
		cerr << "INFO: Cannot receive: Connection not established.\n";
		return 0;
	}
	int recv_data_size = 0;
	while (true) {
		char recv_seg[MAX_SEG_SIZE];
		char send_seg[sizeof(RDTHeader)] = {0};
		memset(recv_seg, 0, MAX_SEG_SIZE);

		// Set up pointers to both the header (hdr) and data (data) portions
		// of the received segment.
		RDTHeader* hdr = (RDTHeader*)recv_seg;
		void *data = (void*)(recv_seg + sizeof(RDTHeader));

		// Receive the data
		int	recv_count = recv(this->sock_fd, recv_seg, MAX_SEG_SIZE, 0);
		// Check if there was an error or a timeout. NOTE: recv should never
		// timeout
		if (recv_count < 0) {
			perror("receive_data recv");
			exit(EXIT_FAILURE);
		}

		cerr << "INFO: Received segment. "
			<< "seq_num = " << ntohl(hdr->sequence_number) << ", "
			<< "ack_num = " << this->sequence_number << ", "
			<< ", type = " << hdr->type << "\n";

			uint32_t seqnum = hdr->sequence_number;

			if (hdr->type == RDT_ACK) {
				// Allow for the sender's ACK to timeout in the case of the
				// initial three way handshake
				continue;
			}
			if (hdr->type == RDT_CLOSE) {
				// Sender initiated the close_connection
				hdr = (RDTHeader*)send_seg;
				hdr->sequence_number = htonl(0);
				hdr->ack_number = htonl(0);
				hdr->type = RDT_ACK;

				// Send an ACK in response to the close message
				this->timeout_send(send_seg);
				
				// Indicate it's on the server side of the connection teardown
				this->state = FIN;
				break;
			} else {
					// Send an ACK for the received data
					hdr = (RDTHeader*)send_seg;
					hdr->ack_number = seqnum;
					hdr->sequence_number = seqnum;
					hdr->type = RDT_ACK;
					if (send(this->sock_fd, send_seg, sizeof(RDTHeader), 0) < 0) {
						perror("receive_data send");
					}

					if (ntohl(seqnum) == this->sequence_number) {
						// Expected sequence number so end the loop
					} else {
							// Out of order sequence number, so drop the data
							continue;
					}
			}
		// Increase the seqnum and output the data
		recv_data_size = recv_count - sizeof(RDTHeader);
		this->sequence_number++;
		memcpy(buffer, data, recv_data_size);
		break;
	}

	return recv_data_size;
}


void ReliableSocket::close_connection() {
	if (this->state != FIN) {
		// Initiating the close_connection
		this->send_close_connection();
	} else {
		// On the receiver side of close_connection	
		this->receive_close_connection();
	}

	// Connection teardown is complete. Close the connection 
	this->state = CLOSED;
	if (close(this->sock_fd) < 0) {
		perror("close_connection close");
	}
	cerr << "Connection successfully closed\n";
}

void ReliableSocket::send_close_connection() {

	char send_seg[MAX_SEG_SIZE] = {0};
	char recv_seg[MAX_SEG_SIZE];

	RDTHeader* hdr = (RDTHeader*)send_seg;
	hdr->ack_number = htonl(0);
	hdr->sequence_number = htonl(0);
	hdr->type = RDT_CLOSE;

	do
	{
			// Send the initial close message
			memset(recv_seg, 0, MAX_SEG_SIZE);
			this->reliable_send(send_seg, sizeof(RDTHeader), recv_seg);
			hdr = (RDTHeader*)recv_seg;
			if (hdr->type == RDT_ACK) {
				break;
			}	
			// Check if ACK was dropped and the server is at the next part in
			// the connection teardown
			if (hdr->type == RDT_CLOSE) {
				break;
			}
	} while (true);

	do
	{
			memset(recv_seg, 0, MAX_SEG_SIZE);
			int	recv_count = recv(this->sock_fd, recv_seg, MAX_SEG_SIZE, 0);
			if (recv_count < 0) {
				// Got a timeout so continue the loop
				continue;
			} else if (recv_count < 0 && errno != EAGAIN) {
				// Error other than a timeout
				perror("recv send_close_connection");
				exit(EXIT_FAILURE);
			}

			hdr = (RDTHeader*)recv_seg;
			if (hdr->type == RDT_CLOSE) {
				// Received the CLOSE so stop the loop
				break;
			}
	} while (true);

	hdr = (RDTHeader*)send_seg;
	hdr->type = RDT_ACK;

	do {
			// Send the final ACK
			if (send(this->sock_fd, send_seg, sizeof(RDTHeader), 0) < 0) {
				// Error occurred while sending
				perror("send_close_connection send");
			}
			// Enter the TIME_WAIT state for the final ACK
			memset(recv_seg, 0, MAX_SEG_SIZE);
			this->set_timeout_length(TIME_WAIT);
			if (recv(this->sock_fd, recv_seg, MAX_SEG_SIZE, 0) > 0) {
				// Recieved a segment while expecting a timeout
				hdr = (RDTHeader*)recv_seg;
				if (hdr->type == RDT_CLOSE) {
					// ACK must have been dropped. Continue loop
					continue;
				}
			} else {
				if (errno == EAGAIN) {
					// Timeout as expected. Connection can close
					break;
				}
				else {
					perror("send_close_connection recv");
					exit(EXIT_FAILURE);
				}
			}
	} while (true);
}

void ReliableSocket::receive_close_connection() {

	char send_seg[MAX_SEG_SIZE] = {0};
	char recv_seg[MAX_SEG_SIZE];

	RDTHeader* hdr = (RDTHeader*)send_seg;
	hdr->ack_number = htonl(0);
	hdr->sequence_number = htonl(0);
	hdr->type = RDT_CLOSE;
	
	// Want to resend RDT_CLOSE if it times out
	this->set_timeout_length(this->estimated_rtt + (4 * this->dev_rtt));

	do
	{
			// Send RDT_CLOSE until receiving final ACK
			memset(recv_seg, 0, MAX_SEG_SIZE);
			this->reliable_send(send_seg, sizeof(RDTHeader), recv_seg);
			hdr = (RDTHeader*)recv_seg;
			if (hdr->type == RDT_ACK) {
				break;
			}
	} while (true);

}
