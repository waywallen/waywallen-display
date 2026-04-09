/*
 * libwaywallen_display — ww_codec unit tests.
 *
 * Exercises the send/recv framing helpers over a socketpair(2) with
 * and without SCM_RIGHTS ancillary fds. Uses `memfd_create(2)` for
 * disposable fds that we can actually inspect after they round-trip.
 */

#define _GNU_SOURCE

#include "codec.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int make_memfd(const char *payload, size_t len) {
    int fd = memfd_create("test_codec", 0);
    if (fd < 0) {
        perror("memfd_create");
        return -1;
    }
    if (len && write(fd, payload, len) != (ssize_t)len) {
        perror("write memfd");
        close(fd);
        return -1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        perror("lseek");
        close(fd);
        return -1;
    }
    return fd;
}

static int read_file(int fd, char *buf, size_t cap) {
    ssize_t n = pread(fd, buf, cap - 1, 0);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

static void test_no_fds_no_body(int a, int b) {
    int rc = ww_codec_send_request(a, 42, NULL, 0, NULL, 0);
    assert(rc == 0);

    uint16_t op = 0;
    uint8_t body_buf[16];
    size_t body_len = 99;
    int fd_buf[4];
    size_t n_fds = 99;
    rc = ww_codec_recv_request(b, &op, body_buf, sizeof(body_buf), &body_len,
                               fd_buf, 4, &n_fds);
    assert(rc == 0);
    assert(op == 42);
    assert(body_len == 0);
    assert(n_fds == 0);
    puts("  ok test_no_fds_no_body");
}

static void test_with_body(int a, int b) {
    static const uint8_t body[] = {1, 2, 3, 4, 5, 6, 7, 8};
    int rc = ww_codec_send_event(a, 7, body, sizeof(body), NULL, 0);
    assert(rc == 0);

    uint16_t op = 0;
    uint8_t body_buf[32];
    size_t body_len = 0;
    int fd_buf[4];
    size_t n_fds = 0;
    rc = ww_codec_recv_event(b, &op, body_buf, sizeof(body_buf), &body_len,
                             fd_buf, 4, &n_fds);
    assert(rc == 0);
    assert(op == 7);
    assert(body_len == sizeof(body));
    assert(memcmp(body_buf, body, sizeof(body)) == 0);
    assert(n_fds == 0);
    puts("  ok test_with_body");
}

static void test_with_fds(int a, int b) {
    /* Three memfds each with distinct payloads. */
    int fd1 = make_memfd("alpha", 5);
    int fd2 = make_memfd("bravo", 5);
    int fd3 = make_memfd("charlie", 7);
    assert(fd1 >= 0 && fd2 >= 0 && fd3 >= 0);
    int send_fds[3] = {fd1, fd2, fd3};

    static const uint8_t body[] = {0xde, 0xad, 0xbe, 0xef};
    int rc = ww_codec_send_event(a, 99, body, sizeof(body), send_fds, 3);
    assert(rc == 0);
    close(fd1);
    close(fd2);
    close(fd3);

    uint16_t op = 0;
    uint8_t body_buf[32];
    size_t body_len = 0;
    int recv_fds[8];
    size_t n_fds = 0;
    rc = ww_codec_recv_event(b, &op, body_buf, sizeof(body_buf), &body_len,
                             recv_fds, 8, &n_fds);
    assert(rc == 0);
    assert(op == 99);
    assert(body_len == 4);
    assert(memcmp(body_buf, body, 4) == 0);
    assert(n_fds == 3);

    /* Read each memfd and verify the payload is preserved. */
    char buf[32];
    assert(read_file(recv_fds[0], buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "alpha") == 0);
    assert(read_file(recv_fds[1], buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "bravo") == 0);
    assert(read_file(recv_fds[2], buf, sizeof(buf)) == 7);
    assert(strcmp(buf, "charlie") == 0);

    close(recv_fds[0]);
    close(recv_fds[1]);
    close(recv_fds[2]);
    puts("  ok test_with_fds");
}

static void test_back_to_back(int a, int b) {
    /* Two frames sent without draining in between. */
    static const uint8_t body1[] = {1};
    static const uint8_t body2[] = {2, 3};
    assert(ww_codec_send_request(a, 10, body1, 1, NULL, 0) == 0);
    assert(ww_codec_send_request(a, 20, body2, 2, NULL, 0) == 0);

    uint16_t op;
    uint8_t buf[8];
    size_t blen;
    int fds[4];
    size_t nfds;

    assert(ww_codec_recv_request(b, &op, buf, sizeof(buf), &blen,
                                 fds, 4, &nfds) == 0);
    assert(op == 10);
    assert(blen == 1);
    assert(buf[0] == 1);

    assert(ww_codec_recv_request(b, &op, buf, sizeof(buf), &blen,
                                 fds, 4, &nfds) == 0);
    assert(op == 20);
    assert(blen == 2);
    assert(buf[0] == 2 && buf[1] == 3);
    puts("  ok test_back_to_back");
}

static void test_peer_closed(int a, int b) {
    close(a);
    uint16_t op;
    uint8_t buf[8];
    size_t blen;
    int fds[4];
    size_t nfds;
    int rc = ww_codec_recv_request(b, &op, buf, sizeof(buf), &blen,
                                   fds, 4, &nfds);
    assert(rc == -ECONNRESET);
    close(b);
    puts("  ok test_peer_closed");
}

static void test_body_too_large(int a, int b) {
    uint8_t body[128];
    memset(body, 0xaa, sizeof(body));
    assert(ww_codec_send_request(a, 1, body, sizeof(body), NULL, 0) == 0);

    uint16_t op;
    uint8_t small_buf[16];
    size_t blen;
    int fds[4];
    size_t nfds;
    int rc = ww_codec_recv_request(b, &op, small_buf, sizeof(small_buf), &blen,
                                   fds, 4, &nfds);
    assert(rc == -EMSGSIZE);
    puts("  ok test_body_too_large");
}

int main(void) {
    puts("test_codec:");

    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        test_no_fds_no_body(sv[0], sv[1]);
        close(sv[0]);
        close(sv[1]);
    }
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        test_with_body(sv[0], sv[1]);
        close(sv[0]);
        close(sv[1]);
    }
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        test_with_fds(sv[0], sv[1]);
        close(sv[0]);
        close(sv[1]);
    }
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        test_back_to_back(sv[0], sv[1]);
        close(sv[0]);
        close(sv[1]);
    }
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        test_peer_closed(sv[0], sv[1]);
    }
    {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        test_body_too_large(sv[0], sv[1]);
        close(sv[0]);
        close(sv[1]);
    }

    puts("test_codec: OK");
    return 0;
}
