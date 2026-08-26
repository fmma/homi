#ifndef HOMI_PROTO_H
#define HOMI_PROTO_H

#include <limits.h>
#include <stdint.h>

#include <libxal.h>

#define HOMI_MAX_CONNECTS   8
#define HOMID_DEVURI_MAXLEN 256

/* xNVMe multi-process group the daemon and its clients share by default, so a
 * stock daemon and a stock client find each other with nothing configured. */
#define HOMI_DEFAULT_SHM_ID 1

enum homi_msg_type {
	HOMI_MSG_TYPE_XAL_CONNECT = 1,  ///< Request xal pool info for a device
	HOMI_MSG_TYPE_XAL_MARK_DIRTY = 5, ///< Flag a device's xal dirty; the daemon re-indexes on its own
	HOMI_MSG_TYPE_LIST_DEVICES = 6,   ///< Ask which devices the daemon serves
};

struct homi_device_info {
	char dev_uri[HOMID_DEVURI_MAXLEN];
	char mountpoint[PATH_MAX]; ///< Filesystem the device holds; empty if it has none
};

/* LIST_DEVICES takes no payload. The reply carries ndevs entries after the
 * header fields, so a client needs no configuration of its own to find the
 * devices and the filesystem each one holds. */
struct homi_res_list_devices {
	int err;
	uint32_t ndevs;
	struct homi_device_info devs[];
};

struct homi_req_xal_connect {
	char dev_uri[HOMID_DEVURI_MAXLEN];
};

struct homi_res_xal_connect {
	int err;
	char shm_name[64];
};

struct homi_req_xal_mark_dirty {
	char dev_uri[HOMID_DEVURI_MAXLEN];
};

struct homi_res_xal_mark_dirty {
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
