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

#ifdef __cplusplus
}
#endif

#endif /* WAYWALLEN_DISPLAY_INTERNAL_CODEC_H */
