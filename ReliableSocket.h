/*
 * File: ReliableSocket.h
 *
 * Header / API file for library that provides reliable data transport over an
 * unreliable link.
 *
 */

// TODO: You'll likely need to add some new types, as you start doing things
// like updating accept_connection and close_connection.
enum RDTMessageType : uint8_t {RDT_SYN, RDT_SYNACK, RDT_ACK, RDT_DATA, RDT_CLOSE};

/**
 * Format for the header of a segment send by our reliable socket.
 */
struct RDTHeader {
	uint32_t sequence_number;
	uint32_t ack_number;
	RDTMessageType type;
};

// TODO: Again, you'll likely need to add new statuses (is that a word?) as
// you start implementing the reliable protocol.
enum connection_status { INIT, ESTABLISHED, FIN, CLOSED};

/**
 * Class that represents a socket using a reliable data transport protocol.
 * This socket uses a stop-and-wait protocol so your data is sent at a nice,
 * leisurely pace.
 */
class ReliableSocket {
public:
	/*
	 * You probably shouldn't add any more public members to this class.
	 * Any new functions or fields you need to add should be private.
	 */
	
	// These are constants for all reliable connections
	static const int MAX_SEG_SIZE  = 1400;
	static const int MAX_DATA_SIZE = MAX_SEG_SIZE - sizeof(RDTHeader);
	static const int TIME_WAIT = 4000; // timed wait for closing the connection
	/**
	 * Basic Constructor, setting estimated RTT to 100 and deviation RTT to 10.
	 */
	ReliableSocket();

	/**
	 * Connects to the specified remote hostname on the given port.
	 *
	 * @param hostname Name of the remote host to connect to.
	 * @param port_num Port number of remote host.
	 */
	void connect_to_remote(char *hostname, int port_num);

	/**
	 * Waits for a connection attempt from a remote host.
	 *
	 * @param port_num The port number to listen on.
	 */
	void accept_connection(int port_num);

	/**
	 * Send data to connected remote host.
	 *
	 * @param buffer The buffer with data to be sent.
	 * @param length The amount of data in the buffer to send.
	 */
	void send_data(const void *buffer, int length);

	/**
	 * Receives data from remote host using a reliable connection.
	 *
	 * @param buffer The buffer where received data will be stored.
	 * @return The amount of data actually received.
	 */
	int receive_data(char buffer[MAX_DATA_SIZE]);

	/**
	 * Closes an connection.
	 */
	void close_connection();

	/**
	 * Returns the estimated RTT.
	 * 
	 * @return Estimated RTT for connection (in milliseconds)
	 */
	uint32_t get_estimated_rtt();
	
private:
	// Private member variables are initialized in the constructor
	int sock_fd;
	uint32_t sequence_number;
	uint32_t expected_sequence_number;
	int estimated_rtt;
	int current_rtt;
	int dev_rtt;
	connection_status state;

	// In the (unlikely?) event you need a new field, add it here.

	/**
	 * Sets the timeout length of this connection.
	 *
	 * @note Setting this to 0 makes the timeout length indefinite (i.e. could
	 * wait forever for a message).
	 *
	 * @param timeout_length_ms Length of timeout period in milliseconds.
	 */
	void set_timeout_length(uint32_t timeout_length_ms);

	/*
	 * Calls set_timeout_length() based on calculated current and dev RTT 
	 *
	 */
	void set_estimated_rtt();

	/*
	 * Calls send() and waits for a message in response, storing it in
	 * recv_seg. Updates timeout length based on the calculated current_rtt of
	 * each send.
	 *
	 * @param *send_seg pointer to the segment to be sent
	 * @param send_seg_size the size of the segment to be sent
	 * @param *recv_seg pointer to the buffer that will store the received msg
	 */
	void reliable_send(char *send_seg, int send_seg_size, char *recv_seg);

	/*
	 * Calls send() and expects it to timeout. If not, the segment will be
	 * resent.
	 * 
	 * @param *send_seg pointer to the segment to be sent
	 */
	void timeout_send(char *send_seg);

	/*
	 * The sender part of closing the connection between sender and receiver
	 *
	 */
	void send_close_connection();

	/*
	 * The receiver part of closing the connection between the sender and
	 * receiver
	 *
	 */
	void receive_close_connection();
};
