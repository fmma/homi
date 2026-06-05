#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libxnvme.h>

#include <homid_log.h>
#include <homid_qpair.h>

#define HOMID_QPAIR_HEAP_BYTES (256ULL * 1024 * 1024)

/* SQ/CQ ring size per queue, matching nvme_qpair_init() in upcie. */
#define HOMID_QPAIR_RING_NBYTES (1024 * 64)

/* First qid homid hands out. xNVMe's upcie owner-mode open creates one sync I/O
 * qpair at qid 1 (the lowest free id), so the pool starts at 2. */
#define HOMID_QID_BASE 2

/**
 * Submit a fully-built admin command on the owner device via xNVMe passthru.
 *
 * With dbuf == NULL the command is submitted verbatim, so the caller's DPTR
 * (prp1) reaches the controller untouched; this is what lets homid point a
 * Create I/O Queue command at a ring it allocated in the shared hugepage.
 */
static int
_pass_admin(struct xnvme_dev *dev, const struct nvme_command *c, void *dbuf, size_t nbytes)
{
	struct xnvme_cmd_ctx ctx = xnvme_cmd_ctx_from_dev(dev);
	int err;

	memcpy(&ctx.cmd, c, sizeof(*c));
	err = xnvme_cmd_pass_admin(&ctx, dbuf, nbytes, NULL, 0);
	if (err) {
		return err;
	}
	if (xnvme_cmd_ctx_cpl_status(&ctx)) {
		return -EIO;
	}

	return 0;
}

/* Register slot's CQ then SQ on the controller (Create I/O Completion/Submission
 * Queue), pointing each at the ring homid allocated in the shared hugepage. */
static int
_create_io_queue(struct homid_qpair_owner *o, struct homid_qpair_slot *s)
{
	struct nvme_command c;
	int err;

	memset(&c, 0, sizeof(c));
	c.opc = 0x5; ///< Create I/O Completion Queue
	c.prp1 = hostmem_dma_v2p(&o->heap, s->cq);
	c.cdw10 = ((uint32_t)(s->depth - 1) << 16) | s->qid;
	c.cdw11 = 0x1; ///< Physically contiguous
	err = _pass_admin(o->dev, &c, NULL, 0);
	if (err) {
		return err;
	}

	memset(&c, 0, sizeof(c));
	c.opc = 0x1; ///< Create I/O Submission Queue
	c.prp1 = hostmem_dma_v2p(&o->heap, s->sq);
	c.cdw10 = ((uint32_t)(s->depth - 1) << 16) | s->qid;
	c.cdw11 = ((uint32_t)s->qid << 16) | 0x1; ///< CQID and physically contiguous
	return _pass_admin(o->dev, &c, NULL, 0);
}

/* Tear slot's queue down on the controller (Delete I/O Submission then
 * Completion Queue; the SQ must go first). */
static int
_delete_io_queue(struct homid_qpair_owner *o, struct homid_qpair_slot *s)
{
	struct nvme_command c;
	int err;

	memset(&c, 0, sizeof(c));
	c.opc = 0x0; ///< Delete I/O Submission Queue
	c.cdw10 = s->qid;
	err = _pass_admin(o->dev, &c, NULL, 0);
	if (err) {
		return err;
	}

	memset(&c, 0, sizeof(c));
	c.opc = 0x4; ///< Delete I/O Completion Queue
	c.cdw10 = s->qid;
	return _pass_admin(o->dev, &c, NULL, 0);
}

/* Reset a slot to a pristine state for reuse: delete its controller-side queue
 * (which resets the controller's SQ-head / CQ-tail / phase), clear the rings,
 * then re-create it over the same rings and qid. */
static int
_recreate_io_queue(struct homid_qpair_owner *o, struct homid_qpair_slot *s)
{
	int err;

	err = _delete_io_queue(o, s);
	if (err) {
		return err;
	}

	memset(s->sq, 0, HOMID_QPAIR_RING_NBYTES);
	memset(s->cq, 0, HOMID_QPAIR_RING_NBYTES);

	return _create_io_queue(o, s);
}

int
homid_qpair_owner_open(struct homid_qpair_owner *o, const char *bdf, uint32_t nsid,
		       uint32_t pool_size, uint16_t depth)
{
	const struct xnvme_spec_idfy_ctrlr *idfy_ctrlr;
	const struct xnvme_spec_idfy_ns *idfy_ns;
	struct xnvme_opts opts = xnvme_opts_default();
	int err;

	if (!o || !bdf || pool_size < 1 || pool_size > UPCIE_ATTACH_MAX_QPAIRS) {
		return -EINVAL;
	}

	memset(o, 0, sizeof(*o));
	o->nsid = nsid;
	snprintf(o->bdf, sizeof(o->bdf), "%s", bdf);

	err = hostmem_config_init(&o->config);
	if (err) {
		homid_log(LOG_ERR, "hostmem_config_init(): %d", err);
		return err;
	}

	/* Shared (memfd) hugepage so clients can map the SQ/CQ rings. */
	err = hostmem_heap_init(&o->heap, HOMID_QPAIR_HEAP_BYTES, &o->config);
	if (err) {
		homid_log(LOG_ERR, "hostmem_heap_init(): %d", err);
		return err;
	}

	/* Owner mode: no XNVME_UPCIE_ATTACH, so xNVMe resets the controller and
	 * owns the admin queue. homid drives the queue lifecycle over it. */
	opts.be = "upcie";
	opts.nsid = nsid;
	o->dev = xnvme_dev_open(o->bdf, &opts);
	if (!o->dev) {
		err = errno ? -errno : -EIO;
		homid_log(LOG_ERR, "xnvme_dev_open(%s, owner): %d", o->bdf, err);
		goto err_heap;
	}

	idfy_ctrlr = xnvme_dev_get_ctrlr(o->dev);
	idfy_ns = xnvme_dev_get_ns(o->dev);
	if (!idfy_ctrlr || !idfy_ns) {
		err = -EIO;
		homid_log(LOG_ERR, "identify payloads unavailable");
		goto err_dev;
	}
	memcpy(o->idfy_ctrlr, idfy_ctrlr, UPCIE_ATTACH_IDFY_NBYTES);
	memcpy(o->idfy_ns, idfy_ns, UPCIE_ATTACH_IDFY_NBYTES);

	o->pool = calloc(pool_size, sizeof(*o->pool));
	o->used = calloc(pool_size, sizeof(*o->used));
	if (!o->pool || !o->used) {
		err = -ENOMEM;
		goto err_pool;
	}

	for (uint32_t i = 0; i < pool_size; i++) {
		struct homid_qpair_slot *s = &o->pool[i];

		s->qid = i + HOMID_QID_BASE;
		s->depth = depth;
		s->sq = hostmem_dma_alloc_array(&o->heap, 1, HOMID_QPAIR_RING_NBYTES);
		s->cq = hostmem_dma_alloc_array(&o->heap, 1, HOMID_QPAIR_RING_NBYTES);
		if (!s->sq || !s->cq) {
			err = -ENOMEM;
			goto err_pool;
		}
		memset(s->sq, 0, HOMID_QPAIR_RING_NBYTES);
		memset(s->cq, 0, HOMID_QPAIR_RING_NBYTES);

		err = _create_io_queue(o, s);
		if (err) {
			/* The controller caps the number of I/O queues (and xNVMe's
			 * owner open already took qid 1), so once it refuses a qid,
			 * stop and serve the pool we secured. Only a failure on the
			 * very first qpair is fatal. */
			if (o->pool_n == 0) {
				homid_log(LOG_ERR, "create_io_queue(qid=%u): %d", s->qid, err);
				goto err_pool;
			}
			homid_log(LOG_NOTICE,
				  "controller accepted %u of %u qpairs (qid=%u refused: %d)",
				  o->pool_n, pool_size, s->qid, err);
			break;
		}
		o->pool_n++;
	}

	pthread_mutex_init(&o->lock, NULL);
	o->opened = 1;

	homid_log(LOG_NOTICE, "qpair owner: %s nsid=%u pool=%u depth=%u region=%s", o->bdf, nsid,
		  o->pool_n, depth, o->heap.memory.path);

	return 0;

err_pool:
	free(o->pool);
	free(o->used);
	o->pool = NULL;
	o->used = NULL;
	o->pool_n = 0;
err_dev:
	xnvme_dev_close(o->dev);
	o->dev = NULL;
err_heap:
	hostmem_heap_term(&o->heap);
	return err;
}

struct xnvme_dev *
homid_qpair_owner_dev(struct homid_qpair_owner *o)
{
	if (!o || !o->opened) {
		return NULL;
	}

	return o->dev;
}

void
homid_qpair_owner_close(struct homid_qpair_owner *o)
{
	if (!o || !o->opened) {
		return;
	}

	pthread_mutex_destroy(&o->lock);
	free(o->pool);
	free(o->used);
	o->pool = NULL;
	o->used = NULL;
	o->pool_n = 0;

	/* Closing the device resets the controller, tearing down every I/O queue;
	 * the heap teardown reclaims the ring memory. */
	xnvme_dev_close(o->dev);
	o->dev = NULL;
	hostmem_heap_term(&o->heap);
	o->opened = 0;
}

int
homid_qpair_owner_handout(struct homid_qpair_owner *o, uint32_t nqpairs,
			  struct upcie_attach_desc *out)
{
	if (!o || !o->opened || !out || nqpairs < 1 || nqpairs > UPCIE_ATTACH_MAX_QPAIRS) {
		return -EINVAL;
	}

	pthread_mutex_lock(&o->lock);

	uint32_t avail = 0;
	for (uint32_t i = 0; i < o->pool_n; i++) {
		if (!o->used[i]) {
			avail++;
		}
	}
	if (avail < nqpairs) {
		pthread_mutex_unlock(&o->lock);
		homid_log(LOG_ERR, "qpair pool exhausted: have %u, free %u, want %u", o->pool_n,
			  avail, nqpairs);
		return -ENOMEM;
	}

	memset(out, 0, sizeof(*out));
	snprintf(out->bdf, sizeof(out->bdf), "%s", o->bdf);
	snprintf(out->region_path, sizeof(out->region_path), "%s", o->heap.memory.path);
	out->region_size = o->heap.memory.size;
	out->nsid = o->nsid;
	out->nqpairs = nqpairs;
	memcpy(out->idfy_ctrlr, o->idfy_ctrlr, UPCIE_ATTACH_IDFY_NBYTES);
	memcpy(out->idfy_ns, o->idfy_ns, UPCIE_ATTACH_IDFY_NBYTES);

	uint32_t given = 0;
	for (uint32_t i = 0; i < o->pool_n && given < nqpairs; i++) {
		struct homid_qpair_slot *s = &o->pool[i];
		struct nvme_qpair_export *e = &out->qpairs[given];

		if (o->used[i]) {
			continue;
		}
		o->used[i] = 1;
		e->qid = s->qid;
		e->depth = s->depth;
		e->_pad = 0;
		e->sq_offset = (uint64_t)((char *)s->sq - (char *)o->heap.memory.virt);
		e->cq_offset = (uint64_t)((char *)s->cq - (char *)o->heap.memory.virt);
		given++;
	}

	pthread_mutex_unlock(&o->lock);

	return 0;
}

void
homid_qpair_owner_reclaim(struct homid_qpair_owner *o, const uint32_t *qids, uint32_t n)
{
	if (!o || !o->opened || !qids) {
		return;
	}

	pthread_mutex_lock(&o->lock);
	for (uint32_t k = 0; k < n; k++) {
		for (uint32_t i = 0; i < o->pool_n; i++) {
			if (o->used[i] && o->pool[i].qid == qids[k]) {
				int err = _recreate_io_queue(o, &o->pool[i]);
				if (err) {
					homid_log(LOG_ERR,
						  "qpair %u recreate failed: %d; retiring slot",
						  qids[k], err);
				} else {
					o->used[i] = 0;
				}
				break;
			}
		}
	}
	pthread_mutex_unlock(&o->lock);
}
