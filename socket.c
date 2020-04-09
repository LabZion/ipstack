
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include "net.h"
#include "socket.h"

void tcp_syn(struct socket_impl *);
void tcp_ack(struct socket_impl *);
void tcp_send(struct socket_impl *, const void *, size_t);
// void tcp_connect(struct socket_impl *);

int min(int a, int b) {
    return a > b ? b : a;
}

#define N_MAXSOCKETS 256

struct socket_impl sockets[N_MAXSOCKETS] = {0};

static int next_avail() {
    int i = -1;
    for (i=0; i<N_MAXSOCKETS; i++) {
        if (!sockets[i].valid) {
            sockets[i].valid = true;
            break;
        }
    }
    return i;
}

int i_socket(int domain, int type, int protocol) {
    int i = next_avail();
    if (i == -1) {
        errno = ENOMEM;
        return -1;
    }

    struct socket_impl *s = sockets + i;

    if (domain != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    // TODO we could validate these inputs
    if (type == SOCK_STREAM) protocol = IPPROTO_TCP;
    if (type == SOCK_DGRAM) protocol = IPPROTO_UDP;

    s->domain = domain;     // AF_INET
    s->type = type;         // SOCK_STREAM, SOCK_DGRAM, or SOCK_RAW
    s->protocol = protocol; // IPPROTO_TCP, IPPROTO_UDP, or IP protocol #
    s->state = SOCKET_REQUESTED;

    if (protocol == IPPROTO_TCP) {
        s->recv_buf = malloc(TCP_RECV_BUF_LEN);
    }

    pthread_mutex_init(&s->block_mtx, NULL);
    pthread_cond_init(&s->block_cond, NULL);

    return i;
}

int i_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    struct socket_impl *s = sockets + sockfd;
    if (!s->valid) {
        errno = ENOTSOCK;
        return -1;
    }

    struct sockaddr_in *in_addr = (struct sockaddr_in *)addr;
    if (in_addr->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    s->local_ip = in_addr->sin_addr.s_addr;
    s->local_port = in_addr->sin_port;

    s->state = SOCKET_BOUND;
    return 0;

}

int i_listen(int sockfd, int backlog) {
    struct socket_impl *s = sockets + sockfd;
    if (!s->valid) {
        errno = ENOTSOCK;
        return -1;
    }

    s->state = SOCKET_LISTENING;
    return 0;
}

int i_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    struct socket_impl *s = sockets + sockfd;
    if (!s->valid) {
        errno = ENOTSOCK;
        return -1;
    }

    if (!(s->type == SOCK_STREAM)) {
        errno = EOPNOTSUPP;
        return -1;
    }

    pthread_mutex_lock(&s->block_mtx);
    struct pkb *accept_pk;
    do {
        pthread_cond_wait(&s->block_cond, &s->block_mtx);
    } while(!list_head_entry(struct pkb, &s->accept_queue, queue));

    accept_pk = list_pop_front(struct pkb, &s->accept_queue, queue);
    struct ip_header *accept_ip = ip_hdr(accept_pk);
    struct tcp_header *accept_tcp = tcp_hdr(accept_ip);
    assert(accept_tcp->f_syn && !accept_tcp->f_ack);

    int i = next_avail();
    if (i == -1) {
        errno = ENOMEM;
        return -1;
    }

    struct socket_impl *as = sockets + i;

    memcpy(as, s, sizeof(struct socket_impl));
    as->state = SOCKET_OUTBOUND;
    as->tcp_state = TCP_S_SYN_RECIEVED;
    as->remote_ip = accept_ip->source_ip;
    as->remote_port = accept_tcp->source_port;
    as->recv_seq = accept_tcp->seq;

    struct sockaddr_in in_addr = {
        .sin_family = AF_INET,
        .sin_port = as->remote_port,
        .sin_addr = {as->remote_ip},
    };

    memcpy(addr, &in_addr, sizeof(in_addr));
    *addrlen = sizeof(in_addr);

    tcp_ack(s);

    pthread_mutex_unlock(&s->block_mtx);
    return i;
}

int tcp_connect(struct socket_impl *s, const struct sockaddr_in *addr) {
    s->remote_ip = addr->sin_addr.s_addr;
    s->remote_port = addr->sin_port;
    s->state = SOCKET_OUTBOUND;
    s->tcp_state = TCP_S_SYN_SENT;

    pthread_mutex_lock(&s->block_mtx);
    tcp_syn(s);

    do {
        pthread_cond_wait(&s->block_cond, &s->block_mtx);
    } while (s->tcp_state == TCP_S_SYN_SENT);

    pthread_mutex_unlock(&s->block_mtx);

    if (s->tcp_state == TCP_S_ESTABLISHED) {
        return 0;
    } else if (s->tcp_state == TCP_S_CLOSED) {
        errno = ECONNREFUSED;
        return -1;
    } else {
        printf("I'm not sure how we got to %i\n", s->tcp_state);
        errno = 1005;
        return -1;
    }
}

int i_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    struct socket_impl *s = sockets + sockfd;
    if (!s->valid) {
        errno = ENOTSOCK;
        return -1;
    }

    const struct sockaddr_in *in_addr = (const struct sockaddr_in *)addr;
    if (in_addr->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    if (s->protocol != IPPROTO_TCP) {
        errno = EPROTO;
        return -1;
    }

    return tcp_connect(s, in_addr);
}

void udp_send(struct socket_impl *s, const void *data, size_t len) {
    if (s->remote_ip == 0) {
        // they should use sendto
        return; // indicate error?
    }

    struct pkb *pk = new_pk();
    make_udp(s, pk, NULL, data, len);
    dispatch(pk);
    free_pk(pk);

    s->ip_id += 1;
}

ssize_t i_send(int sockfd, const void *buf, size_t len, int flags) {
    struct socket_impl *s = sockets + sockfd;
    if (!s->valid) {
        errno = ENOTSOCK;
        return -1;
    }

    if (s->state != SOCKET_OUTBOUND) {
        errno = EDESTADDRREQ;
        return -1;
    }

    if (s->type == SOCK_DGRAM) {
        udp_send(s, buf, len);
    }

    if (s->type == SOCK_STREAM) {
        tcp_send(s, buf, len);
    }
    return len;
}

void udp_sendto(struct socket_impl *s, struct sockaddr_in *d_addr,
        const void *data, size_t len) {
    struct pkb *pk = new_pk();
    make_udp(s, pk, d_addr, data, len);
    dispatch(pk);
    free_pk(pk);

    s->ip_id += 1;
}

ssize_t i_sendto(int sockfd, const void *buf, size_t len, int flags,
        const struct sockaddr *dest_addr, socklen_t addrlen) {
    struct socket_impl *s = sockets + sockfd;
    if (!s->valid) {
        errno = ENOTSOCK;
        return -1;
    }

    if (s->type != SOCK_DGRAM) {
        errno = EFAULT; // TODO
        return -1;
    }

    struct sockaddr_in *in_addr = (struct sockaddr_in *)dest_addr;
    if (addrlen != sizeof(*in_addr)) {
        errno = EFAULT; // TODO ? How do you handle this?
        return -1;
    }

    udp_sendto(s, in_addr, buf, len);
    return len; // what if we sent less?
}

ssize_t i_recv(int sockfd, void *buf, size_t len, int flags) {
    printf("I_RECV\n");
    struct socket_impl *s = sockets + sockfd;
    if (!s->valid) {
        errno = ENOTSOCK;
        return -1;
    }

    size_t buf_ix = 0;

    while (buf_ix < len) {
        pthread_mutex_lock(&s->block_mtx);
        pthread_cond_wait(&s->block_cond, &s->block_mtx);
        pthread_mutex_unlock(&s->block_mtx);

        if (s->tcp_state != TCP_S_ESTABLISHED) {
            // done
            return 0;
        }

        size_t available = min(len - buf_ix, s->recv_buf_len);

        if (available > 0) {
            memcpy(buf + buf_ix, s->recv_buf, available);
            buf_ix += available;

            memmove(s->recv_buf, s->recv_buf + available, available);
            s->recv_buf_seq += available;
            s->recv_buf_len -= available;
        }

        if (s->tcp_psh) {
            s->tcp_psh = false;
            break;
        }
    }

    return buf_ix;
}

ssize_t i_recvfrom(int sockfd, void *buf, size_t len, int flags,
        struct sockaddr *src_addr, socklen_t *addrlen) {
    struct socket_impl *s = sockets + sockfd;
    if (!s->valid) {
        errno = ENOTSOCK;
        return -1;
    }

    pthread_mutex_lock(&s->block_mtx);
    struct pkb *recv_pk;
    do {
        pthread_cond_wait(&s->block_cond, &s->block_mtx);
    } while (!list_head_entry(struct pkb, &s->dgram_queue, queue));

    recv_pk = list_pop_front(struct pkb, &s->dgram_queue, queue);
    struct ip_header *recv_ip = ip_hdr(recv_pk);
    struct udp_header *recv_udp = udp_hdr(recv_ip);
    int udp_data_len = udp_len(recv_pk);
    void *udp_d = udp_data(recv_pk);

    struct sockaddr_in *in_addr = (struct sockaddr_in *)src_addr;
    if (*addrlen < sizeof(*in_addr)) {
        errno = EFAULT;
        return -1;
    }
    in_addr->sin_family = AF_INET;
    in_addr->sin_port = recv_udp->source_port;
    in_addr->sin_addr.s_addr = recv_ip->source_ip;
    *addrlen = sizeof(*in_addr);

    len = (udp_data_len < len) ? udp_data_len : len;
    memcpy(buf, udp_d, len);

    pthread_mutex_unlock(&s->block_mtx);
    return len;
}

int i_close(int sockfd) {
    struct socket_impl *s = sockets + sockfd;
    if (!s->valid) return -1;
    
    // teardown connections ?

    s->state = SOCKET_IDLE;
    s->valid = false;
    return 0;
}

//
// DISPATCH
//

void socket_dispatch_udp(struct pkb *pk) {
    printf("dispatching udp\n");
    struct ip_header *ip = ip_hdr(pk);
    struct udp_header *udp = udp_hdr(ip);

    int best_match = -1;
    for (int i=0; i<N_MAXSOCKETS; i++) {
        int strength = 0;
        struct socket_impl *s = sockets + i;

        if (!(s->valid)) continue;
        if (!(s->domain == AF_INET)) continue;
        if (!(s->type == SOCK_DGRAM)) continue;
        if (!(s->protocol == IPPROTO_UDP)) continue;

        if (s->local_ip == ip->destination_ip) strength++;
        if (s->local_port == udp->destination_port) strength++;

        if (s->state == SOCKET_BOUND && s->local_ip == 0) strength++;

        if (s->state == SOCKET_OUTBOUND) {
            if (s->remote_ip == ip->source_ip) strength++;
            if (s->remote_port == udp->source_port) strength++;
        }

        if (s->state == SOCKET_BOUND && strength == 2) {
            best_match = i;
            break;
        }

        if (s->state == SOCKET_OUTBOUND && strength == 4) {
            best_match = i;
            break;
        }
    }
    if (best_match == -1) {
        // no matching socket, drop.
        return;
    }
    printf("dispatch found match: %i\n", best_match);
    struct socket_impl *s = sockets + best_match;

    list_append(&s->dgram_queue, pk, queue);

    pthread_mutex_lock(&s->block_mtx);
    pthread_cond_signal(&s->block_cond);
    pthread_mutex_unlock(&s->block_mtx);
}

void tcp_syn(struct socket_impl *s) {
    s->send_seq = rand();
    s->send_ack = 0;
    s->recv_seq = 0;

    struct pkb *pk = new_pk();
    make_tcp(s, pk, TCP_SYN, NULL, 0);
    dispatch(pk);
    free_pk(pk);

    s->ip_id += 1;
    s->send_seq += 1; // SYN is ~ 1 byte.
    s->tcp_state = TCP_S_SYN_SENT;
}

void tcp_ack(struct socket_impl *s) {
    enum tcp_flags flags = TCP_ACK;
    if (s->tcp_state == TCP_S_LISTEN) {
        flags |= TCP_SYN;
    }

    struct pkb *pk = new_pk();
    make_tcp(s, pk, flags, NULL, 0);
    dispatch(pk);

    struct ip_header *ip = ip_hdr(pk);
    struct tcp_header *tcp = tcp_hdr(ip);

    s->ip_id += 1;
    if (tcp->f_syn || tcp->f_fin) {
        s->send_seq += 1;
    }
    free_pk(pk);
}

void tcp_fin_ack(struct socket_impl *s) {
    enum tcp_flags flags = TCP_FIN | TCP_ACK;

    struct pkb *pk = new_pk();
    make_tcp(s, pk, flags, NULL, 0);
    dispatch(pk);

    struct ip_header *ip = ip_hdr(pk);
    struct tcp_header *tcp = tcp_hdr(ip);

    s->ip_id += 1;
    if (tcp->f_syn || tcp->f_fin) {
        s->send_seq += 1;
    }
    free_pk(pk);
}

void tcp_send(struct socket_impl *s, const void *data, size_t len) {
    struct pkb *pk = new_pk();
    make_tcp(s, pk, TCP_PSH | TCP_ACK, data, len);
    dispatch(pk);
    free_pk(pk);

    s->ip_id += 1;
    s->send_seq += len;
}

void require_that(bool x) {}

void socket_dispatch_tcp(struct pkb *pk) {
    struct ip_header *ip = ip_hdr(pk);
    struct tcp_header *tcp = tcp_hdr(ip);

    int best_match = -1;
    for (int i=0; i<N_MAXSOCKETS; i++) {
        int strength = 0;
        struct socket_impl *s = sockets + i;

        if (!(s->valid)) continue;
        if (!(s->domain == AF_INET)) continue;
        if (!(s->type == SOCK_STREAM)) continue;
        if (!(s->protocol == IPPROTO_TCP)) continue;

        if (s->local_ip == ip->destination_ip) strength++;
        if (s->local_port == tcp->destination_port) strength++;

        if (s->state == SOCKET_LISTENING && s->local_ip == 0) strength++;

        if (s->state == SOCKET_OUTBOUND) {
            if (s->remote_ip == ip->source_ip) strength++;
            if (s->remote_port == tcp->source_port) strength++;
        }

        if (s->state == SOCKET_LISTENING && strength == 2) {
            best_match = i;
            break;
        }

        if (s->state == SOCKET_OUTBOUND && strength == 4) {
            best_match = i;
            break;
        }
    }
    if (best_match == -1) {
        // no matching socket, drop.
        return;
    }
    printf("dispatch found match: %i\n", best_match);
    struct socket_impl *s = sockets + best_match;

    uint32_t tcp_rseq = ntohl(tcp->seq);
    uint32_t tcp_rack = ntohl(tcp->ack);

    // ACK sequence update
    if (tcp->f_ack) {
        s->send_ack = tcp_rack;
    }

    // RST -> close
    if (s->tcp_state == TCP_S_SYN_SENT && tcp->f_rst) {
        s->tcp_state = TCP_S_CLOSED;
        pthread_mutex_lock(&s->block_mtx);
        pthread_cond_signal(&s->block_cond);
        pthread_mutex_unlock(&s->block_mtx);
    }

    // SYN/ACK -> Established
    if (s->tcp_state == TCP_S_SYN_SENT && tcp->f_syn && tcp->f_ack) {
        require_that(s->send_ack == s->send_seq);
        s->recv_seq = tcp_rseq + 1; // SYN is ~ 1 byte
        tcp_ack(s);

        s->tcp_state = TCP_S_ESTABLISHED;

        pthread_mutex_lock(&s->block_mtx);
        pthread_cond_signal(&s->block_cond);
        pthread_mutex_unlock(&s->block_mtx);
    }

    // FIN -> FIN/ACK
    if (s->tcp_state == TCP_S_ESTABLISHED && tcp->f_fin) {
        s->recv_seq = tcp_rseq + 1; // FIN counts as a byte
        tcp_fin_ack(s);

        s->tcp_state = TCP_S_CLOSE_WAIT;

        pthread_mutex_lock(&s->block_mtx);
        pthread_cond_signal(&s->block_cond);
        pthread_mutex_unlock(&s->block_mtx);
    }

    if (s->tcp_state == TCP_S_CLOSING && tcp->f_ack) {
        // do I know the ack is _for_ the FIN ?
        //
        // also is this CLOSING or LAST_ACK ?
    }

    uint32_t start_rseq = tcp_rseq;
    uint32_t end_rseq = tcp_rseq + tcp_len(pk);

    if (tcp->f_syn || tcp->f_fin) {
        end_rseq += 1;
    }

    if (s->tcp_state == TCP_S_ESTABLISHED) {
        // TODO ^^ MODULO 2**32

        if (s->recv_seq == start_rseq) {
            int tcp_length = tcp_len(pk);
            if (tcp_length + s->recv_buf_len > TCP_RECV_BUF_LEN) {
                // too slow!
                // TODO: tcp_ooo_insert(ooo_queue)
                //       s->pending_slow_data = true
            } else {
                void *tcp_d = tcp_data(pk);
                memcpy(s->recv_buf + s->recv_buf_len, tcp_d, tcp_length);
                s->recv_buf_len += tcp_length;

                s->recv_seq = end_rseq;
                tcp_ack(s);

                /*
                if (list_head_entry(&s->ooo_queue)) {
                    // TODO: try to apply ooo data pending
                }
                */

                // TODO: check if needs signal?
                // not sure how this works tbh

                if (tcp->f_psh) {
                    // signal userspace should process ASAP
                    s->tcp_psh = true;
                }
                pthread_mutex_lock(&s->block_mtx);
                pthread_cond_signal(&s->block_cond);
                pthread_mutex_unlock(&s->block_mtx);
            }
        }
    }

    // RST
    if (tcp->f_rst) {
        printf("TCP RST\n");
        s->tcp_state = TCP_S_CLOSED;
        return;
    }

    if (s->state == SOCKET_LISTENING) {
        list_append(&s->accept_queue, pk, queue);

        pthread_mutex_lock(&s->block_mtx);
        pthread_cond_signal(&s->block_cond);
        pthread_mutex_unlock(&s->block_mtx);

        // accept runs the syn/ack
    }
}

