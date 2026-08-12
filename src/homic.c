#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <libxal.h>

#include <homic.h>
#include <homi_proto.h>

struct homic_xal_entry {
	struct xal *xal;
	char dev_uri[HOMID_DEVURI_MAXLEN];
};

struct homic_client {
	char *socket_path;
	size_t xal_count;
	struct homic_xal_entry *xals;
	char qpair_dev_uri[HOMID_DEVURI_MAXLEN];
	int qpair_sock_fd;
};

static struct homic_client *g_homic_client = NULL;

static int
_connect(char *socket_path)
{
	struct sockaddr_un saddr;
	int sock_fd, err;

	sock_fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		err = -errno;
		fprintf(stderr, "Failed: socket(); err(%d)\n", err);
		return err;
	}

	saddr.sun_family = AF_LOCAL;
	strncpy(saddr.sun_path, socket_path, sizeof(saddr.sun_path));
	saddr.sun_path[sizeof(saddr.sun_path) - 1] = '\0';

	err = connect(sock_fd, (struct sockaddr *)&saddr, sizeof(saddr));
	if (err) {
		err = -errno;
		fprintf(stderr, "Failed: connect(); err(%d)\n", err);
		close(sock_fd);
		return err;
	}

	return sock_fd;
}

int
homic_connect(char *socket_path)
{
	struct homic_client *cand;
	int sock_fd, err;

	cand = calloc(1, sizeof(*cand));
	if (!cand) {
		err = -errno;
		fprintf(stderr, "Failed: calloc(); err(%d)\n", err);
		return err;
	}

	sock_fd = _connect(socket_path);
	if (sock_fd < 0) {
		err = sock_fd;
		goto failed;
	}
	close(sock_fd);

	cand->socket_path = strdup(socket_path);
	if (!cand->socket_path) {
		err = -ENOMEM;
		goto failed;
	}
	cand->qpair_sock_fd = -1;

	g_homic_client = cand;

	return 0;

failed:
	free(cand);
	return err;
}

void
homic_disconnect()
{
	if (!g_homic_client) {
		return;
	}

	homic_detach_qpair();

	free(g_homic_client->socket_path);

	for (size_t i = 0; i < g_homic_client->xal_count; i++) {
		xal_close(g_homic_client->xals[i].xal);
	}
	free(g_homic_client->xals);

	free(g_homic_client);
	g_homic_client = NULL;
}

int
homic_connect_xal(char *dev_uri, struct xal **out)
{
	struct homi_msg_header hdr = {0};
	struct homi_req_xal_connect req = {0};
	struct homi_res_xal_connect *res = NULL;
	struct homic_xal_entry *new_xals;
	char shm_name[64];
	size_t new_count;
	int sock_fd = -1, err;

	if (!g_homic_client) {
		err = -ENOTCONN;
		fprintf(stderr, "Failed: No connection, please call homic_connect(); err(%d)\n", err);
		return err;
	}
	if (strlen(dev_uri) >= HOMID_DEVURI_MAXLEN) {
		return -EINVAL;
	}

	sock_fd = _connect(g_homic_client->socket_path);
	if (sock_fd < 0) {
		err = sock_fd;
		fprintf(stderr, "Failed: _connect(%s); err(%d)\n", g_homic_client->socket_path, err);
		goto exit;
	}

	strncpy(req.dev_uri, dev_uri, sizeof(req.dev_uri) - 1);
	hdr.type = HOMI_MSG_TYPE_XAL_CONNECT;

	err = homi_proto_socket_write(sock_fd, &hdr, &req, sizeof(req));
	if (err) {
		fprintf(stderr, "Failed: homi_proto_socket_write(); err(%d)\n", err);
		goto exit;
	}

	err = homi_proto_socket_read(sock_fd, &hdr, (void **)&res);
	if (err) {
		fprintf(stderr, "Failed: homi_proto_socket_read(); err(%d)\n", err);
		goto exit;
	}
	if (res->err) {
		err = res->err;
		fprintf(stderr, "Failed: daemon xal_connect error; err(%d)\n", err);
		goto exit;
	}

	close(sock_fd);
	sock_fd = -1;

	/* Copy out of shm before any further operations touch the segment. */
	memcpy(shm_name, res->shm_name, sizeof(shm_name));

	err = xal_from_shm(shm_name, out);
	if (err) {
		fprintf(stderr, "Failed: xal_from_shm(); err(%d)\n", err);
		goto exit;
	}

	new_count = g_homic_client->xal_count + 1;
	new_xals = realloc(g_homic_client->xals, new_count * sizeof(*g_homic_client->xals));
	if (!new_xals) {
		err = -ENOMEM;
		xal_close(*out);
		*out = NULL;
		goto exit;
	}
	g_homic_client->xals = new_xals;

	new_xals[new_count - 1].xal = *out;
	strcpy(new_xals[new_count - 1].dev_uri, dev_uri);
	g_homic_client->xal_count = new_count;

exit:
	free(res);

	if (sock_fd >= 0) {
		close(sock_fd);
	}

	return err;
}

int
homic_attach_qpair(char *dev_uri, unsigned nqpairs, char **out_descpath)
{
	struct homi_msg_header hdr = {0};
	struct homi_req_qpair_attach req = {0};
	struct homi_res_qpair_attach *res;
	char *payload = NULL;
	char path[256];
	int sock_fd = -1, fd = -1, err;

	if (!g_homic_client) {
		fprintf(stderr, "Failed: No connection, please call homic_connect()\n");
		return -ENOTCONN;
	}
	if (!dev_uri || !out_descpath) {
		return -EINVAL;
	}

	sock_fd = _connect(g_homic_client->socket_path);
	if (sock_fd < 0) {
		return sock_fd;
	}

	strncpy(req.dev_uri, dev_uri, sizeof(req.dev_uri) - 1);
	req.nqpairs = nqpairs;
	hdr.type = HOMI_MSG_TYPE_QPAIR_ATTACH;

	err = homi_proto_socket_write(sock_fd, &hdr, &req, sizeof(req));
	if (err) {
		goto exit;
	}

	err = homi_proto_socket_read(sock_fd, &hdr, (void **)&payload);
	if (err) {
		goto exit;
	}

	if (hdr.payload_len < sizeof(*res)) {
		err = -EIO;
		goto exit;
	}
	res = (struct homi_res_qpair_attach *)payload;
	if (res->err) {
		err = res->err;
		fprintf(stderr, "Failed: daemon qpair_attach error; err(%d)\n", err);
		goto exit;
	}
	if (res->desc_len == 0 || hdr.payload_len < sizeof(*res) + res->desc_len) {
		err = -EIO;
		goto exit;
	}

	snprintf(g_homic_client->qpair_dev_uri, sizeof(g_homic_client->qpair_dev_uri), "%s",
		 dev_uri);

	/* Write the opaque attach descriptor to a file for XNVME_UPCIE_ATTACH. */
	snprintf(path, sizeof(path), "/run/homi/qpair-%d.desc", (int)getpid());
	fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd < 0) {
		err = -errno;
		fprintf(stderr, "Failed: open(%s); err(%d)\n", path, err);
		goto exit;
	}
	{
		const char *p = payload + sizeof(*res);
		size_t left = res->desc_len, done = 0;

		while (done < left) {
			ssize_t n = write(fd, p + done, left - done);
			if (n < 0) {
				err = -errno;
				goto exit;
			}
			done += (size_t)n;
		}
	}
	close(fd);
	fd = -1;

	*out_descpath = strdup(path);
	err = *out_descpath ? 0 : -ENOMEM;
	if (!err) {
		g_homic_client->qpair_sock_fd = sock_fd;
		sock_fd = -1;
	}

exit:
	if (fd >= 0) {
		close(fd);
	}
	free(payload);
	if (sock_fd >= 0) {
		close(sock_fd);
	}
	return err;
}

int
homic_detach_qpair(void)
{
	if (!g_homic_client) {
		return -ENOTCONN;
	}
	if (g_homic_client->qpair_sock_fd < 0) {
		return 0;
	}

	close(g_homic_client->qpair_sock_fd);
	g_homic_client->qpair_sock_fd = -1;
	g_homic_client->qpair_dev_uri[0] = '\0';

	return 0;
}

int
homic_mark_dirty(char *dev_uri)
{
	struct homi_msg_header hdr = {0};
	struct homi_req_xal_mark_dirty req = {0};
	struct homi_res_xal_mark_dirty *res = NULL;
	int sock_fd = -1, err;

	if (!g_homic_client) {
		err = -ENOTCONN;
		fprintf(stderr, "Failed: No connection, please call homic_connect(); err(%d)\n", err);
		return err;
	}

	sock_fd = _connect(g_homic_client->socket_path);
	if (sock_fd < 0) {
		err = sock_fd;
		fprintf(stderr, "Failed: _connect(%s); err(%d)\n", g_homic_client->socket_path, err);
		goto exit;
	}

	strncpy(req.dev_uri, dev_uri, sizeof(req.dev_uri) - 1);
	hdr.type = HOMI_MSG_TYPE_XAL_MARK_DIRTY;

	err = homi_proto_socket_write(sock_fd, &hdr, &req, sizeof(req));
	if (err) {
		fprintf(stderr, "Failed: homi_proto_socket_write(); err(%d)\n", err);
		goto exit;
	}

	err = homi_proto_socket_read(sock_fd, &hdr, (void **)&res);
	if (err) {
		fprintf(stderr, "Failed: homi_proto_socket_read(); err(%d)\n", err);
		goto exit;
	}

	err = res->err;
	if (err) {
		fprintf(stderr, "Failed: daemon xal_mark_dirty error; err(%d)\n", err);
	}

exit:
	free(res);
	if (sock_fd >= 0) {
		close(sock_fd);
	}
	return err;
}

static int
_xal_for_uri(char *dev_uri, struct xal **out)
{
	for (size_t i = 0; i < g_homic_client->xal_count; i++) {
		if (strcmp(g_homic_client->xals[i].dev_uri, dev_uri) == 0) {
			*out = g_homic_client->xals[i].xal;
			return 0;
		}
	}

	return homic_connect_xal(dev_uri, out);
}

int
homic_get_extents(int fd, struct homic_extent **out, uint32_t *n)
{
	struct xal *xal = NULL;
	struct xal_extents *ex = NULL;
	struct homic_extent *arr;
	char fdpath[64], path[PATH_MAX];
	ssize_t plen;
	int err;

	if (fd < 0 || !out || !n) {
		return -EINVAL;
	}
	if (!g_homic_client) {
		fprintf(stderr, "Failed: No connection, please call homic_connect()\n");
		return -ENOTCONN;
	}
	if (g_homic_client->qpair_dev_uri[0] == '\0') {
		fprintf(stderr, "Failed: No attached device, please call homic_attach_qpair()\n");
		return -EINVAL;
	}

	snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
	plen = readlink(fdpath, path, sizeof(path) - 1);
	if (plen < 0) {
		err = -errno;
		fprintf(stderr, "Failed: readlink(%s); err(%d)\n", fdpath, err);
		return err;
	}
	path[plen] = '\0';

	err = _xal_for_uri(g_homic_client->qpair_dev_uri, &xal);
	if (err) {
		return err;
	}

	/* The daemon rewrites the shared inode/extent pools in place when the
	 * filesystem changes, concurrent with this read. Treat the pools as a
	 * seqlock snapshot: an odd seq means a rewrite is in progress, and the
	 * seq changing across the read means the pools moved under us. A dirty
	 * flag means the filesystem changed but is not yet re-indexed. In any of
	 * those cases the extents we would return are inconsistent or stale, so
	 * report -ESTALE and let the caller retry once the daemon re-indexes. */
	int seq = xal_get_seq_lock(xal);
	if ((seq & 1) || xal_is_dirty(xal)) {
		return -ESTALE;
	}

	err = xal_get_extents(xal, path, &ex);
	if (err) {
		/* A torn read during a rewrite can surface as a spurious lookup
		 * failure; only trust the error if the snapshot held. */
		if (xal_get_seq_lock(xal) != seq || xal_is_dirty(xal)) {
			return -ESTALE;
		}
		fprintf(stderr, "Failed: xal_get_extents('%s'); err(%d)\n", path, err);
		return err;
	}

	/* ex points into the shared inode pool, so count/extent_idx may be torn.
	 * Capture them, then validate the snapshot before use: if the seq held,
	 * these are values the daemon wrote and are in range by construction. */
	uint32_t count = ex->count;
	uint32_t base = ex->extent_idx;

	atomic_thread_fence(memory_order_acquire);
	if (xal_get_seq_lock(xal) != seq || xal_is_dirty(xal)) {
		return -ESTALE;
	}

	arr = calloc(count ? count : 1, sizeof(*arr));
	if (!arr) {
		return -ENOMEM;
	}

	for (uint32_t k = 0; k < count; k++) {
		struct xal_extent *e = xal_extent_at(xal, base + k);
		struct xal_extent_converted in_bytes = {0}, in_lba = {0};

		err = xal_extent_in_bytes(xal, e, &in_bytes);
		if (err) {
			goto read_failed;
		}
		err = xal_extent_in_lba(xal, e, &in_lba);
		if (err) {
			goto read_failed;
		}

		arr[k].file_offset = in_bytes.start_offset;
		arr[k].length = in_bytes.size;
		arr[k].slba = in_lba.start_block;
	}

	/* Validate the snapshot: order the pool reads above before re-reading the
	 * seq, then reject if a rewrite ran or the filesystem dirtied at any point
	 * across the copy. */
	atomic_thread_fence(memory_order_acquire);
	if (xal_get_seq_lock(xal) != seq || xal_is_dirty(xal)) {
		free(arr);
		return -ESTALE;
	}

	*out = arr;
	*n = count;

	return 0;

read_failed:
	free(arr);
	if (xal_get_seq_lock(xal) != seq || xal_is_dirty(xal)) {
		return -ESTALE;
	}
	return err;
}
