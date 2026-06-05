#ifndef HOMI_PROTO_H
#define HOMI_PROTO_H

#include <stdint.h>

#include <libxal.h>

#define HOMI_MAX_CONNECTS   8
#define HOMID_DEVURI_MAXLEN 256

/* Upper bound on qpairs named in one ATTACH/DETACH message. Kept in sync with
 * upcie's UPCIE_ATTACH_MAX_QPAIRS so the proto header stays free of the upcie
 * include. */
#define HOMI_QPAIR_MAX 16

enum homi_msg_type {
	HOMI_MSG_TYPE_XAL_CONNECT = 1,  ///< Request xal pool info for a device
	HOMI_MSG_TYPE_QPAIR_ATTACH = 2, ///< Request a slice of a device's I/O qpair pool
	HOMI_MSG_TYPE_QPAIR_DETACH = 3, ///< Return previously attached qpairs to the pool
};

struct homi_req_xal_connect {
	char dev_uri[HOMID_DEVURI_MAXLEN];
};

struct homi_res_xal_connect {
	int err;
	char shm_name[64];
};

struct homi_req_qpair_attach {
	char dev_uri[HOMID_DEVURI_MAXLEN];
	uint32_t nqpairs; ///< Number of I/O qpairs requested
};

/**
 * Response header for HOMI_MSG_TYPE_QPAIR_ATTACH.
 *
 * On success (err == 0) the response payload is this header followed by
 * `desc_len` bytes of an opaque attach descriptor: exactly the file content the
 * xNVMe upcie backend reads when XNVME_UPCIE_ATTACH points at it. Clients treat
 * those bytes as opaque and need not interpret them.
 */
struct homi_res_qpair_attach {
	int err;
	uint32_t desc_len;
	uint32_t nqpairs;            ///< Number of valid entries in qids[]
	uint32_t qids[HOMI_QPAIR_MAX]; ///< I/O queue ids handed out, for later DETACH
};

struct homi_req_qpair_detach {
	char dev_uri[HOMID_DEVURI_MAXLEN];
	uint32_t nqpairs;            ///< Number of valid entries in qids[]
	uint32_t qids[HOMI_QPAIR_MAX]; ///< I/O queue ids to return to the pool
};

struct homi_res_qpair_detach {
	int err;
};

struct homi_msg_header {
	enum homi_msg_type type;
	size_t payload_len;
};

/**
 * Read a message from a socket.
 *
 * Reads a homi_msg_header followed by its payload from sock_fd. The payload
 * is heap-allocated and returned via *buf; the caller is responsible for
 * freeing it. *buf is set to NULL if payload_len is zero.
 *
 * @param sock_fd  File descriptor of the connected socket.
 * @param hdr      Output: populated with the received message header.
 * @param buf      Output: allocated buffer containing the payload, or NULL.
 * @return         0 on success, negative errno on failure.
 */
int
homi_proto_socket_read(int sock_fd, struct homi_msg_header *hdr, void **buf);

/**
 * Write a message to a socket.
 *
 * Sends hdr followed by buf as a single framed message. Sets hdr->payload_len
 * to buf_len before writing.
 *
 * @param sock_fd   File descriptor of the connected socket.
 * @param hdr       Message header; payload_len will be overwritten with buf_len.
 * @param buf       Payload to send.
 * @param buf_len   Length of the payload in bytes.
 * @return          0 on success, negative errno on failure.
 */
int
homi_proto_socket_write(int sock_fd, struct homi_msg_header *hdr, void *buf, size_t buf_len);

#endif /* HOMI_PROTO_H */
