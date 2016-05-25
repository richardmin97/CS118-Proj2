#ifndef TCPManager_h
#define TCPManager_h

#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>

int custom_listen(int sockfd, int backlog);
int custom_accept(int sockfd, struct sockaddr *addr, socklen_t* addrlen, int flags);
int custom_connect(int sockfd, const struct sockaddr * addr, socklen_t addrlen);
int custom_recv(int sockfd, void* buf, size_t len, int flags);
int custom_send(int sockfd, void* buf, size_t len, int flags);


struct packet_headers {
	uint16_t h_seq;
	uint16_t h_ack;
	uint16_t h_window;
	uint16_t flags;
};

#define ACK_FLAG 0x100
#define SYN_FLAG 0x10
#define FIN_FLAG 0x1

#endif