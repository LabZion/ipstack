
#ifndef IPSTACK_SOCKET_H
#define IPSTACK_SOCKET_H

#include <sys/types.h>
#include <sys/socket.h>
#include "core.h"
#include "net.h"

typedef uint32_t be32;
typedef uint16_t be16;

enum socket_state {
    SOCKET_IDLE = 0,
    SOCKET_REQUESTED,
    SOCKET_BOUND,
    SOCKET_LISTENING,
    SOCKET_OUTBOUND,
};

enum tcp_state {
    TCP_S_LISTEN,
    TCP_S_SYN_SENT,
    TCP_S_SYN_RECIEVED,
    TCP_S_ESTABLISHED,
    TCP_S_FIN_WAIT_1,
    TCP_S_FIN_WAIT_2,
    TCP_S_CLOSE_WAIT,
    TCP_S_CLOSING,
    TCP_S_LAST_ACK,
    TCP_S_TIME_WAIT,
    TCP_S_CLOSED,
};

struct socket_impl {
    bool valid;
    enum socket_state state;

    int domain;
    int type;
    int protocol;

    pthread_mutex_t block_mtx;
    pthread_cond_t block_cond;

    // IP {{
    unsigned int ip_id;
    be32 local_ip;
    be16 local_port;
    be32 remote_ip;
    be16 remote_port;
    // }}

    // SOCK_DGRAM {{
    list dgram_queue;  // datagram socket pks
    // }}

    // TCP {{
    list accept_queue; // accept() TCP_SYN pks

    uint32_t send_seq; // SND.NXT - seq of next byte to send
    uint32_t send_ack; // SND.UNA - seq of last byte sent acknowleged
    uint32_t recv_seq; // RCV.NXT - seq of next byte to be recieved
    uint16_t window_size;

    enum tcp_state tcp_state;

#define TCP_RECV_BUF_LEN (64 * 1024)
    uint32_t recv_buf_seq;
    size_t recv_buf_len;
    char *recv_buf;

    bool tcp_psh;

    // packets with seq > next expected seq.
    list ooo_queue;
    // packets that could be retransmitted if needed
    list unacked_pks;
    // }}
};

int i_socket(int domain, int type, int protocol);
int i_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int i_listen(int sockfd, int backlog);
int i_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int i_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t i_send(int sockfd, const void *buf, size_t len, int flags);
ssize_t i_sendto(int sockfd, const void *buf, size_t len, int flags,
        const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t i_recv(int sockfd, void *bud, size_t len, int flags);
ssize_t i_recvfrom(int sockfd, void *buf, size_t len, int flags,
        struct sockaddr *src_addr, socklen_t *addrlen);

// int x_socket(int domain, int type, int protocol);
int x_bind(struct socket_impl *, const struct sockaddr *addr, socklen_t addrlen);
int x_listen(struct socket_impl *, int backlog);
int x_accept(struct socket_impl *, struct sockaddr *addr, socklen_t *addrlen);
int x_connect(struct socket_impl *, const struct sockaddr *addr, socklen_t addrlen);
ssize_t x_send(struct socket_impl *, const void *buf, size_t len, int flags);
ssize_t x_sendto(struct socket_impl *, const void *buf, size_t len, int flags,
        const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t x_recv(struct socket_impl *, void *bud, size_t len, int flags);
ssize_t x_recvfrom(struct socket_impl *, void *buf, size_t len, int flags,
        struct sockaddr *src_addr, socklen_t *addrlen);

void socket_dispatch_udp(struct pkb *);
void socket_dispatch_tcp(struct pkb *);

#endif // IPSTACK_SOCKET_H

