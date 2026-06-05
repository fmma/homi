#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <homid.h>
#include <homid_ipc.h>
#include <homid_log.h>
#include <homid_qpair.h>
#include <homid_xal.h>
#include <homi_proto.h>

struct worker_args {
	int client_fd;
	struct homid *homid;
};

static int
_open_socket(char *socket_path)
{
	struct sockaddr_un saddr;
	int fd, err = 0;

	fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (fd < 0) {
		homid_log(LOG_ERR, "Failed: socket(); err(%d)", errno);
		return -errno;
	}

	saddr.sun_family = AF_LOCAL;
	strncpy(saddr.sun_path, socket_path, sizeof(saddr.sun_path));
	saddr.sun_path[sizeof(saddr.sun_path) - 1] = '\0';

	err = bind(fd, (struct sockaddr *)&saddr, sizeof(saddr));
	if (err) {
		homid_log(LOG_ERR, "Failed: bind(); err(%d)", errno);
		close(fd);
		return -errno;
	}

	return fd;
}

int
homid_ipc_open(char *socket_path, struct homid_ipc_connection **conn)
{
	struct homid_ipc_connection *cand;
	int fd, err = 0;

	cand = calloc(1, sizeof(*cand));
	if (!cand) {
		err = -errno;
		homid_log(LOG_CRIT, "Failed: calloc(); errno(%d)", err);
		return err;
	}

	fd = _open_socket(socket_path);
	if (fd < 0) {
		err = fd;
		homid_log(LOG_ERR, "Failed: _open_socket(); err(%d)", err);
		goto failed;
	}
	cand->fd = fd;

	err = listen(fd, HOMI_MAX_CONNECTS);
	if (err) {
		err = -errno;
		homid_log(LOG_ERR, "Failed: listen(); err(%d)", err);
		goto failed;
	}

	homid_log(LOG_INFO, "Listening for client connections ...");

	*conn = cand;

	return 0;

failed:
	homid_ipc_close(cand);

	return err;
}

void
homid_ipc_close(struct homid_ipc_connection *conn)
{
	if (!conn) {
		homid_log(LOG_INFO, "No homid_ipc_connection given; skipping homid_ipc_close()");
		return;
	}

	if (conn->fd >= 0) {
		close(conn->fd);
	}
}

static void *
worker(void *arg)
{
	struct worker_args *wargs = arg;
	struct homid *homid = wargs->homid;
	int sock_fd = wargs->client_fd;
	struct homi_msg_header hdr;
	void *payload = NULL;
	int err;

	free(wargs);

	err = homi_proto_socket_read(sock_fd, &hdr, &payload);
	if (err) {
		homid_log(LOG_ERR, "Failed: homi_proto_socket_read(hdr); err(%d)", err);
		goto exit;
	}

	switch ((enum homi_msg_type)hdr.type) {
	case HOMI_MSG_TYPE_XAL_CONNECT:
		struct homi_req_xal_connect *req = (struct homi_req_xal_connect *)payload;
		struct homi_res_xal_connect res = {0};
		struct homid_device *device = NULL;

		if (!req) {
			homid_log(LOG_ERR, "Error: Payload required for XAL_CONNECT request");
			res.err = -EINVAL;
			goto send_response;
		}

		device = homid_device_get(homid, req->dev_uri);

		if (!device) {
			homid_log(LOG_ERR, "XAL_CONNECT: device not found: %s", req->dev_uri);
			res.err = -ENODEV;
			goto send_response;
		}

		memcpy(res.shm_name, device->shm_name, sizeof(res.shm_name));

send_response:
		err = homi_proto_socket_write(sock_fd, &hdr, &res, sizeof(res));
		if (err) {
			homid_log(LOG_ERR, "Failed: homi_proto_socket_write(); err(%d)", err);
		}

		break;

	case HOMI_MSG_TYPE_QPAIR_ATTACH: {
		struct homi_req_qpair_attach *qreq = payload;
		struct homi_res_qpair_attach qres = {0};
		struct homid_device *qdev;
		struct upcie_attach_desc *qdesc = NULL;
		char *qbuf = NULL;
		size_t qbuf_len;
		uint32_t nqp;

		if (!qreq) {
			homid_log(LOG_ERR, "QPAIR_ATTACH: payload required");
			qres.err = -EINVAL;
			goto qpair_reply_err;
		}

		nqp = qreq->nqpairs ? qreq->nqpairs : 1;
		qdev = homid_device_get(homid, qreq->dev_uri);
		if (!qdev || !qdev->qpo) {
			homid_log(LOG_ERR, "QPAIR_ATTACH: no owned device: %s", qreq->dev_uri);
			qres.err = -ENODEV;
			goto qpair_reply_err;
		}

		qdesc = calloc(1, sizeof(*qdesc));
		if (!qdesc) {
			qres.err = -ENOMEM;
			goto qpair_reply_err;
		}

		qres.err = homid_qpair_owner_handout(qdev->qpo, nqp, qdesc);
		if (qres.err) {
			free(qdesc);
			goto qpair_reply_err;
		}

		qres.nqpairs = qdesc->nqpairs;
		for (uint32_t qi = 0; qi < qdesc->nqpairs && qi < HOMI_QPAIR_MAX; qi++) {
			qres.qids[qi] = qdesc->qpairs[qi].qid;
		}
		qres.desc_len = (uint32_t)sizeof(*qdesc);
		qbuf_len = sizeof(qres) + qres.desc_len;
		qbuf = malloc(qbuf_len);
		if (!qbuf) {
			free(qdesc);
			qres.err = -ENOMEM;
			qres.desc_len = 0;
			goto qpair_reply_err;
		}
		memcpy(qbuf, &qres, sizeof(qres));
		memcpy(qbuf + sizeof(qres), qdesc, sizeof(*qdesc));
		free(qdesc);

		err = homi_proto_socket_write(sock_fd, &hdr, qbuf, qbuf_len);
		free(qbuf);
		if (err) {
			homid_log(LOG_ERR, "QPAIR_ATTACH write: %d", err);
		}
		break;

	qpair_reply_err:
		homi_proto_socket_write(sock_fd, &hdr, &qres, sizeof(qres));
		break;
	}

	case HOMI_MSG_TYPE_QPAIR_DETACH: {
		struct homi_req_qpair_detach *dreq = payload;
		struct homi_res_qpair_detach dres = {0};
		struct homid_device *ddev;
		uint32_t dn;

		if (!dreq) {
			homid_log(LOG_ERR, "QPAIR_DETACH: payload required");
			dres.err = -EINVAL;
			goto detach_reply;
		}

		ddev = homid_device_get(homid, dreq->dev_uri);
		if (!ddev || !ddev->qpo) {
			homid_log(LOG_ERR, "QPAIR_DETACH: no owned device: %s", dreq->dev_uri);
			dres.err = -ENODEV;
			goto detach_reply;
		}

		dn = dreq->nqpairs < HOMI_QPAIR_MAX ? dreq->nqpairs : HOMI_QPAIR_MAX;
		homid_qpair_owner_reclaim(ddev->qpo, dreq->qids, dn);

	detach_reply:
		err = homi_proto_socket_write(sock_fd, &hdr, &dres, sizeof(dres));
		if (err) {
			homid_log(LOG_ERR, "QPAIR_DETACH write: %d", err);
		}
		break;
	}

	default:
		homid_log(LOG_WARNING, "Unknown message type: %u", hdr.type);
		break;
	}

exit:
	free(payload);
	close(sock_fd);
	return NULL;
}

int
homid_ipc_accept(struct homid *homid)
{
	struct homid_ipc_connection *conn;
	struct worker_args *wargs;
	struct sockaddr addr;
	pthread_t thr_id;
	uint32_t len;
	int client_fd, err;

	if (!homid) {
		homid_log(LOG_ERR, "Error: No homid struct given");
		return -EINVAL;
	}

	conn = homid->conn;

	len = sizeof(addr);

	homid_log(LOG_DEBUG, "Waiting for incoming connections...\n");
	client_fd = accept(conn->fd, &addr, &len);

	if (client_fd < 0) {
		homid_log(LOG_WARNING, "Failed: accept(); continuing");
		return 0;
	}

	wargs = calloc(1, sizeof(*wargs));
	if (!wargs) {
		err = -errno;
		homid_log(LOG_CRIT, "Failed: calloc(); err(%d)", err);
		close(client_fd);
		return err;
	}

	wargs->client_fd = client_fd;
	wargs->homid = homid;

	err = pthread_create(&thr_id, NULL, worker, wargs);
	if (err) {
		homid_log(LOG_ERR, "Failed: pthread_create(); err(%d)", err);
		free(wargs);
		close(client_fd);
		return err;
	}

	return 0;
}
