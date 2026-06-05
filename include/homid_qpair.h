#ifndef HOMID_QPAIR_H
#define HOMID_QPAIR_H

#include <pthread.h>
#include <stdint.h>
#include <sys/uio.h> /* struct iovec, used by <upcie/nvme/nvme_request.h> */

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

struct xnvme_dev;

/**
 * One pre-created I/O qpair slot.
 *
 * homid allocates the SQ/CQ rings from the shared hugepage itself and registers
 * the queue on the controller via the admin queue; it only needs the qid, depth,
 * and ring addresses to build a handout descriptor and to delete/recreate the
 * queue on reclaim. The rings are never driven by homid, only handed out.
 */
struct homid_qpair_slot {
	uint32_t qid;
	uint16_t depth;
	void *sq; ///< SQ ring VA within the shared heap
	void *cq; ///< CQ ring VA within the shared heap
};

/**
 * Controller-ownership state for one NVMe device.
 *
 * homid opens the controller in userspace via xNVMe's upcie backend (owner
 * mode), owning the admin queue, and pre-creates a pool of I/O qpairs whose
 * SQ/CQ rings live in a shared (memfd) hugepage. The queue lifecycle (create,
 * delete, recreate-to-reset) is driven through xnvme_cmd_pass_admin; only the
 * shared ring memory comes from upcie/hostmem directly. homid hands slices of
 * the pool to clients by filling a upcie_attach_desc, which the xNVMe upcie
 * backend imports in attach mode. The controller is never handed out; only I/O
 * qpairs are.
 */
struct homid_qpair_owner {
	struct hostmem_config config;
	struct hostmem_heap heap; ///< Shared hugepage heap backing the rings
	struct xnvme_dev *dev;    ///< Owner-mode device; its admin queue drives the queue lifecycle

	struct homid_qpair_slot *pool; ///< Pre-created I/O qpair slots
	uint32_t pool_n;               ///< Number of qpairs created
	uint8_t *used;                 ///< Per-qpair in-use flag; handout sets, reclaim clears
	pthread_mutex_t lock;          ///< Guards the used[] map across IPC worker threads

	char bdf[32];
	uint32_t nsid;
	uint8_t idfy_ctrlr[UPCIE_ATTACH_IDFY_NBYTES];
	uint8_t idfy_ns[UPCIE_ATTACH_IDFY_NBYTES];

	int opened;
};

/**
 * Open the controller at `bdf` and pre-create `pool_size` I/O qpairs.
 *
 * Resets the controller, sets up the admin queue, captures Identify
 * Controller/Namespace for namespace `nsid`, and creates the qpair pool from a
 * shared hugepage. The device must be unbound from the kernel nvme driver.
 *
 * @return 0 on success, negative errno on failure.
 */
int
homid_qpair_owner_open(struct homid_qpair_owner *o, const char *bdf, uint32_t nsid,
		       uint32_t pool_size, uint16_t depth);

void
homid_qpair_owner_close(struct homid_qpair_owner *o);

/**
 * Return the owner-mode xnvme device backing the controller.
 *
 * homid drives the queue lifecycle over this dev and reuses it for xal reads;
 * it belongs to the owner and must not be closed by the caller.
 *
 * @return the device, or NULL if the owner is not open.
 */
struct xnvme_dev *
homid_qpair_owner_dev(struct homid_qpair_owner *o);

/**
 * Hand out `nqpairs` free qpairs from the pool into a fresh attach descriptor.
 *
 * Fills `out` (bdf, shared-region path, geometry, Identify payloads, and
 * `nqpairs` qpair exports taken from free pool slots, which are marked in-use).
 * Thread-safe. Return the qpairs with homid_qpair_owner_reclaim() once the
 * client is done with them.
 *
 * @return 0 on success, -ENOMEM if too few qpairs are free, -EINVAL on bad args.
 */
int
homid_qpair_owner_handout(struct homid_qpair_owner *o, uint32_t nqpairs,
			  struct upcie_attach_desc *out);

/**
 * Return previously handed-out qpairs to the pool.
 *
 * Marks each pool slot whose qid appears in `qids` as free again, so a later
 * handout can reuse it. Unknown or already-free qids are ignored. Thread-safe.
 */
void
homid_qpair_owner_reclaim(struct homid_qpair_owner *o, const uint32_t *qids, uint32_t n);

#endif /* HOMID_QPAIR_H */
