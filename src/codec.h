/*
 * Internal wire codec for libwaywallen_display.
 *
 * Frames the `waywallen-display-v1` protocol header (u16 opcode,
 * u16 total_length including header) over a blocking SOCK_STREAM
 * Unix domain socket. SCM_RIGHTS ancillary fds ride the same
 * sendmsg/recvmsg call as the first 4 header bytes, mirroring the
 * Rust side's `display_proto::codec` contract.
 *
 * All functions return 0 on success or a negative errno-style code
 * on failure. `-ECONNRESET` is returned when the peer closes the
 * connection mid-frame. `-EMSGSIZE` is returned when the caller's
 * supplied buffer is too small for the body or when the frame
 * carries more ancillary fds than `fd_cap`. In the latter case all
 * delivered fds (including those that fit) are closed before return.
 *
 * This header is not installed — it's an implementation detail of
 * the library and not part of the public ABI.
 */

#ifndef WAYWALLEN_DISPLAY_INTERNAL_CODEC_H
#define WAYWALLEN_DISPLAY_INTERNAL_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>  /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

#define WW_CODEC_MAX_BODY_BYTES  ((size_t)65531)  /* u16 max - 4 header */
#define WW_CODEC_MAX_FDS_PER_MSG ((size_t)64)

int ww_codec_send_request(int fd,
                          uint16_t op,
                          const uint8_t *body, size_t body_len,
                          const int *fds, size_t n_fds);

int ww_codec_send_event(int fd,
                        uint16_t op,
                        const uint8_t *body, size_t body_len,
                        const int *fds, size_t n_fds);

int ww_codec_recv_request(int fd,
                          uint16_t *op,
                          uint8_t *body_buf, size_t body_cap, size_t *body_len,
                          int *fd_buf, size_t fd_cap, size_t *n_fds);

int ww_codec_recv_event(int fd,
                        uint16_t *op,
                        uint8_t *body_buf, size_t body_cap, size_t *body_len,
                        int *fd_buf, size_t fd_cap, size_t *n_fds);

/* ------------------------------------------------------------------ */
/* Non-blocking partial-frame primitives                              */
/*                                                                    */
/* For event-loop-driven callers (handshake state machine, future     */
/* dispatch refactor). Caller must use a non-blocking fd; primitives  */
/* additionally pass MSG_DONTWAIT defensively.                        */
/* ------------------------------------------------------------------ */

#define WW_CODEC_FRAME_DONE  1  /* full frame in `st`; caller decodes */
#define WW_CODEC_FRAME_NEED  2  /* fd not ready, arm POLLIN, retry    */

/*
 * State bag for incremental recv of one framed event/request. Reusable
 * across many frames via reset(). Carries the parsed opcode, body
 * bytes, and any SCM_RIGHTS fds harvested across partial reads.
 */
typedef struct ww_codec_recv_state {
    uint8_t  hdr[4];
    size_t   hdr_filled;       /* 0..4 */
    uint16_t op;               /* valid once hdr_filled == 4         */
    size_t   body_len;         /* valid once hdr_filled == 4         */
    uint8_t  body[WW_CODEC_MAX_BODY_BYTES];
    size_t   body_filled;
    int      fds[WW_CODEC_MAX_FDS_PER_MSG];
    size_t   n_fds;
} ww_codec_recv_state_t;

void ww_codec_recv_state_init (ww_codec_recv_state_t *st);
/* Close any unclaimed fds and zero the state. Call after handing the
 * frame to the caller (who took the fds it wanted). */
void ww_codec_recv_state_reset(ww_codec_recv_state_t *st);

/*
 * Non-blocking single-shot receive. Returns:
 *   WW_CODEC_FRAME_DONE — st has a complete frame; consume + reset.
 *   WW_CODEC_FRAME_NEED — recv would block; rearm POLLIN and retry.
 *   <0                  — fatal -errno (incl. -ECONNRESET on peer EOF).
 */
int ww_codec_recv_partial(int fd, ww_codec_recv_state_t *st);

/*
 * Non-blocking single sendmsg of raw bytes (no SCM_RIGHTS). Returns:
 *   >0      — bytes written (caller advances and re-sends if < len).
 *   0       — EAGAIN/EWOULDBLOCK; rearm POLLOUT and retry.
 *   <0      — fatal -errno.
 *
 * Handshake frames carry no fds, so a plain send() suffices. For
 * fd-bearing frames keep using ww_codec_send_event/_request.
 */
ssize_t ww_codec_send_partial(int fd, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WAYWALLEN_DISPLAY_INTERNAL_CODEC_H */
