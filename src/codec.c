/*
 * Wire codec implementation. See codec.h for the contract.
 */

#define _GNU_SOURCE  /* MSG_CMSG_CLOEXEC */

#include "codec.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/* Control-message scratch big enough for MAX_FDS_PER_MSG. Used by
 * both send and recv paths. */
#define WW_CMSG_SPACE CMSG_SPACE(sizeof(int) * WW_CODEC_MAX_FDS_PER_MSG)

#ifndef MSG_CMSG_CLOEXEC
/* Fall back: POSIX doesn't have MSG_CMSG_CLOEXEC. Receiving fds
 * without it is subject to a theoretical fork/exec race, but the
 * library only runs on Linux in practice. */
#define MSG_CMSG_CLOEXEC 0
#endif

/* ------------------------------------------------------------------ */
/*  Send path                                                          */
/* ------------------------------------------------------------------ */

static int do_send(int fd,
                   uint16_t op,
                   const uint8_t *body, size_t body_len,
                   const int *fds, size_t n_fds) {
    if (fd < 0) return -EBADF;
    if (body_len > WW_CODEC_MAX_BODY_BYTES) return -EMSGSIZE;
    if (n_fds > WW_CODEC_MAX_FDS_PER_MSG) return -EMSGSIZE;
    if (n_fds > 0 && !fds) return -EINVAL;

    size_t total = 4u + body_len;
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(op & 0xff);
    hdr[1] = (uint8_t)((op >> 8) & 0xff);
    hdr[2] = (uint8_t)(total & 0xff);
    hdr[3] = (uint8_t)((total >> 8) & 0xff);

    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = 4;
    int iovcnt = 1;
    if (body_len > 0) {
        /* sendmsg wants a non-const pointer; cast away const — the
         * kernel never writes to msg_iov buffers. */
        iov[1].iov_base = (void *)(uintptr_t)body;
        iov[1].iov_len = body_len;
        iovcnt = 2;
    }

    /* cmsg buffer sized for MAX_FDS; actual used length is smaller. */
    union {
        char raw[WW_CMSG_SPACE];
        struct cmsghdr align;
    } cmsg_store;
    memset(&cmsg_store, 0, sizeof(cmsg_store));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = (size_t)iovcnt;

    if (n_fds > 0) {
        msg.msg_control = cmsg_store.raw;
        msg.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * n_fds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * n_fds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * n_fds);
    }

    for (;;) {
        ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if ((size_t)n != total) {
            /* SOCK_STREAM sendmsg on a UDS is supposed to send the
             * whole frame atomically if the kernel buffer has room;
             * partial writes are unexpected and we treat them as IO
             * failure rather than trying to resume (which would need
             * to re-attach the cmsg, impossible). */
            return -EIO;
        }
        return 0;
    }
}

int ww_codec_send_request(int fd, uint16_t op,
                          const uint8_t *body, size_t body_len,
                          const int *fds, size_t n_fds) {
    return do_send(fd, op, body, body_len, fds, n_fds);
}

int ww_codec_send_event(int fd, uint16_t op,
                        const uint8_t *body, size_t body_len,
                        const int *fds, size_t n_fds) {
    return do_send(fd, op, body, body_len, fds, n_fds);
}

/* ------------------------------------------------------------------ */
/*  Receive path                                                       */
/* ------------------------------------------------------------------ */

static void close_all(int *fds, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

/* Blocking read(2) that fills the whole buffer, handling short reads
 * and EINTR. Returns 0 on success, -errno on failure. */
static int read_exact(int fd, void *buf, size_t want) {
    uint8_t *p = (uint8_t *)buf;
    while (want > 0) {
        ssize_t n = read(fd, p, want);
        if (n == 0) return -ECONNRESET;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        p += (size_t)n;
        want -= (size_t)n;
    }
    return 0;
}

static int do_recv(int fd,
                   uint16_t *op_out,
                   uint8_t *body_buf, size_t body_cap, size_t *body_len_out,
                   int *fd_buf, size_t fd_cap, size_t *n_fds_out) {
    if (fd < 0) return -EBADF;
    if (!op_out || !body_len_out || !n_fds_out) return -EINVAL;
    if (body_cap > 0 && !body_buf) return -EINVAL;
    if (fd_cap > 0 && !fd_buf) return -EINVAL;

    *body_len_out = 0;
    *n_fds_out = 0;

    uint8_t hdr[4];
    size_t hdr_filled = 0;
    size_t received_fds = 0;

    /* Phase 1: read the 4-byte header via recvmsg, harvesting any
     * SCM_RIGHTS ancillary data that rode the first byte of the
     * frame. SOCK_STREAM may return fewer than 4 bytes in one call;
     * loop until the header is complete. */
    while (hdr_filled < 4) {
        struct iovec iov;
        iov.iov_base = hdr + hdr_filled;
        iov.iov_len = 4 - hdr_filled;

        union {
            char raw[WW_CMSG_SPACE];
            struct cmsghdr align;
        } cmsg_store;
        memset(&cmsg_store, 0, sizeof(cmsg_store));

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_store.raw;
        msg.msg_controllen = (socklen_t)sizeof(cmsg_store.raw);

        ssize_t n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            close_all(fd_buf, received_fds);
            return -errno;
        }

        /* Harvest SCM_RIGHTS *before* checking for EOF — the kernel
         * may attach cmsg to zero-byte recvmsg calls in rare edge
         * cases, but more importantly we want to drain them exactly
         * once regardless of whether the payload arrived. */
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL;
             c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                size_t payload_len =
                    (size_t)c->cmsg_len - (size_t)CMSG_LEN(0);
                size_t nh = payload_len / sizeof(int);
                const unsigned char *src = (const unsigned char *)CMSG_DATA(c);
                for (size_t i = 0; i < nh; i++) {
                    int got;
                    memcpy(&got, src + i * sizeof(int), sizeof(int));
                    if (received_fds < fd_cap) {
                        fd_buf[received_fds++] = got;
                    } else {
                        /* Overflow: close this and every remaining
                         * ancillary fd in this cmsg, then close the
                         * fds we already captured. */
                        close(got);
                        for (size_t j = i + 1; j < nh; j++) {
                            int extra;
                            memcpy(&extra, src + j * sizeof(int), sizeof(int));
                            close(extra);
                        }
                        close_all(fd_buf, received_fds);
                        return -EMSGSIZE;
                    }
                }
            }
        }

        if (n == 0) {
            close_all(fd_buf, received_fds);
            return -ECONNRESET;
        }
        hdr_filled += (size_t)n;
    }

    uint16_t op = (uint16_t)((uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8));
    uint16_t total = (uint16_t)((uint32_t)hdr[2] | ((uint32_t)hdr[3] << 8));
    if (total < 4) {
        close_all(fd_buf, received_fds);
        return -EPROTO;
    }
    size_t body_len = (size_t)(total - 4u);
    if (body_len > body_cap) {
        close_all(fd_buf, received_fds);
        return -EMSGSIZE;
    }

    if (body_len > 0) {
        int rc = read_exact(fd, body_buf, body_len);
        if (rc < 0) {
            close_all(fd_buf, received_fds);
            return rc;
        }
    }

    *op_out = op;
    *body_len_out = body_len;
    *n_fds_out = received_fds;
    return 0;
}

int ww_codec_recv_request(int fd, uint16_t *op,
                          uint8_t *body_buf, size_t body_cap, size_t *body_len,
                          int *fd_buf, size_t fd_cap, size_t *n_fds) {
    return do_recv(fd, op, body_buf, body_cap, body_len, fd_buf, fd_cap, n_fds);
}

int ww_codec_recv_event(int fd, uint16_t *op,
                        uint8_t *body_buf, size_t body_cap, size_t *body_len,
                        int *fd_buf, size_t fd_cap, size_t *n_fds) {
    return do_recv(fd, op, body_buf, body_cap, body_len, fd_buf, fd_cap, n_fds);
}

/* ------------------------------------------------------------------ */
/*  Non-blocking partial-frame primitives                              */
/* ------------------------------------------------------------------ */

void ww_codec_recv_state_init(ww_codec_recv_state_t *st) {
    memset(st, 0, sizeof(*st));
    for (size_t i = 0; i < WW_CODEC_MAX_FDS_PER_MSG; i++) st->fds[i] = -1;
}

void ww_codec_recv_state_reset(ww_codec_recv_state_t *st) {
    /* Close any unclaimed fds — caller owns harvested fds only after it
     * explicitly copies them out before reset. */
    close_all(st->fds, st->n_fds);
    ww_codec_recv_state_init(st);
}

int ww_codec_recv_partial(int fd, ww_codec_recv_state_t *st) {
    if (fd < 0) return -EBADF;
    if (!st)    return -EINVAL;

    /* Phase 1: header. SCM_RIGHTS rides the first byte of the frame
     * per the kernel's UDS contract; we harvest cmsg on every recvmsg
     * defensively (cf. do_recv comment in this file). */
    while (st->hdr_filled < 4) {
        struct iovec iov;
        iov.iov_base = st->hdr + st->hdr_filled;
        iov.iov_len  = 4 - st->hdr_filled;

        union {
            char raw[WW_CMSG_SPACE];
            struct cmsghdr align;
        } cmsg_store;
        memset(&cmsg_store, 0, sizeof(cmsg_store));

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov         = &iov;
        msg.msg_iovlen      = 1;
        msg.msg_control     = cmsg_store.raw;
        msg.msg_controllen  = (socklen_t)sizeof(cmsg_store.raw);

        ssize_t n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return WW_CODEC_FRAME_NEED;
            return -errno;
        }

        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL;
             c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                size_t payload_len =
                    (size_t)c->cmsg_len - (size_t)CMSG_LEN(0);
                size_t nh = payload_len / sizeof(int);
                const unsigned char *src = (const unsigned char *)CMSG_DATA(c);
                for (size_t i = 0; i < nh; i++) {
                    int got;
                    memcpy(&got, src + i * sizeof(int), sizeof(int));
                    if (st->n_fds < WW_CODEC_MAX_FDS_PER_MSG) {
                        st->fds[st->n_fds++] = got;
                    } else {
                        close(got);
                        for (size_t j = i + 1; j < nh; j++) {
                            int extra;
                            memcpy(&extra, src + j * sizeof(int), sizeof(int));
                            close(extra);
                        }
                        ww_codec_recv_state_reset(st);
                        return -EMSGSIZE;
                    }
                }
            }
        }

        if (n == 0) {
            ww_codec_recv_state_reset(st);
            return -ECONNRESET;
        }
        st->hdr_filled += (size_t)n;
    }

    /* Header complete — (re-)parse op + body_len. Idempotent so it's
     * safe to recompute on each entry. */
    st->op = (uint16_t)(st->hdr[0] | ((uint16_t)st->hdr[1] << 8));
    size_t total = (size_t)st->hdr[2] | ((size_t)st->hdr[3] << 8);
    if (total < 4) {
        ww_codec_recv_state_reset(st);
        return -EBADMSG;
    }
    if (total - 4 > WW_CODEC_MAX_BODY_BYTES) {
        ww_codec_recv_state_reset(st);
        return -EMSGSIZE;
    }
    st->body_len = total - 4;

    /* Phase 2: body. fds were already drained in phase 1; plain recv. */
    while (st->body_filled < st->body_len) {
        ssize_t n = recv(fd, st->body + st->body_filled,
                         st->body_len - st->body_filled, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return WW_CODEC_FRAME_NEED;
            return -errno;
        }
        if (n == 0) {
            ww_codec_recv_state_reset(st);
            return -ECONNRESET;
        }
        st->body_filled += (size_t)n;
    }

    return WW_CODEC_FRAME_DONE;
}

ssize_t ww_codec_send_partial(int fd, const uint8_t *buf, size_t len) {
    if (fd < 0) return -EBADF;
    if (len > 0 && !buf) return -EINVAL;
    if (len == 0) return 0;
    for (;;) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -errno;
    }
}
