#ifndef HOMID_XAL_H
#define HOMID_XAL_H

#include <stdbool.h>

#include <homi_proto.h>
#include <homid_opts.h>

struct homid;
struct homid_qpair_owner;

struct homid_device {
	struct xnvme_dev *dev;
	struct xal *xal;
	bool watching;
	char uri[HOMID_DEVURI_MAXLEN];
	char shm_name[64];

	/* upcie controller ownership: homid owns the controller and hands out
	 * I/O qpairs from a shared pool. `dev` is the owner-mode controller dev,
	 * also used for xal reads. */
	struct homid_qpair_owner *qpo;
};

/**
 * Setup xal for the homid_device
 *
 * For the given homid_device, open xal and retrieve extents through a full scan.
 * xnvme device must be initialized first.
 *
 * @param opts		xal_opts parsed from config file.
 * @param device	Output: device to setup.
 * @return			0 on success, negative errno on failure.
 */
int
homid_xal_setup(struct xal_opts *opts, struct homid_device *device);

/**
 * Re-index a device's xal from the live filesystem.
 *
 * Re-runs the FIEMAP scan and rewrites the shared inode/extent pools, clearing
 * the dirty flag. Serialized internally. The caller must keep the filesystem
 * quiescent for the duration; readers resolving extents concurrently may see a
 * partially rewritten view.
 *
 * @param device  Device whose xal is re-indexed (must already be set up).
 * @return        0 on success, -EAGAIN if not yet indexed, negative errno on
 *                failure.
 */
int
homid_xal_reindex(struct homid_device *device);

/**
 * Setup xnvme for the homid_device
 *
 * For the given homid_device, initialize xnvme.
 * Attaches to homid's own controller (device->qpo must be opened first) and
 * opens device->dev in upcie attach mode for xal to read over.
 *
 * @param device	Device whose ->dev is opened (->qpo must be set).
 * @return			0 on success, negative errno on failure.
 */
int
homid_xnvme_setup(struct homid_device *device);

/**
 * Cleans up array of homid_device
 *
 * For the given number of devices, clean each homid_device's xal and xvnme setups.
 * Checks before freeing.
 *
 * @param ndevs		Number of devices to clean.
 * @param devices	Array of homid_device to clean.
 */
void
homid_device_close(unsigned int ndevs, struct homid_device *devices);

/**
 * Setup array of homid_device
 *
 * Allocates array of homid_device and initializes xnvme and xal for each of them.
 * Uses device uri, ndevs, and xal_opts from user config options.
 *
 * @param opts		config options parsed from user config file.
 * @param devices	Output: array of homid_device to setup.
 * @return			0 on success, negative errno on failure.
 */
int
homid_device_setup(struct homid_opts *opts, struct homid_device **devices);

/**
 * Get a pointer to the homid_device from a given device URI
 *
 * @param homid   Daemon state
 * @param uri     Device URI
 * @return        A pointer to the first homid_device with a matching URI, NULL if
 *                none is found.
 */
struct homid_device *
homid_device_get(struct homid *homid, char *uri);

/**
 * Start the background xal indexer.
 *
 * Spawns a thread that waits for opts->mountpoint to be mounted (the qublk
 * filesystem only appears once a client attaches a qpair, so it cannot exist
 * at daemon startup), then indexes each device's xal over that mount and
 * publishes the shm. Call after the IPC socket is bound; the qpair pool is
 * already serving, so qublk can attach and create the mount being waited on.
 *
 * @param homid  Daemon state holding the devices to index.
 * @param opts   xal options; opts->mountpoint selects the filesystem to FIEMAP.
 */
void
homid_xal_index_start(struct homid *homid, struct xal_opts *opts);

/**
 * Join the background xal indexer thread.
 *
 * The indexer breaks its wait on the global stop flag (set on SIGTERM/SIGINT).
 * No-op if the indexer was never started.
 */
void
homid_xal_index_stop(void);

#endif /* HOMID_XAL_H */
