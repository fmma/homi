#ifndef HOMID_XAL_H
#define HOMID_XAL_H

#include <stdbool.h>
#include <stdint.h>

#include <homi_proto.h>
#include <homid_opts.h>

struct homid;

struct homid_device {
	/* Primary-mode device in the shared multi-process group: the daemon
	 * brings the controller up, holds it up, and reads filesystem metadata
	 * over it for xal. */
	struct xnvme_dev *dev;
	struct xal *xal;
	bool watching;
	char uri[HOMID_DEVURI_MAXLEN];
	char shm_name[64];
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
 * the dirty flag. Serialized internally and invoked by the xal watch thread on
 * a filesystem change. Concurrent readers detect the in-place rewrite via the
 * seqlock and get -ESTALE, so no quiescing is required.
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
 * Opens device->uri as the primary of the `shm_id` multi-process group, which
 * brings the controller up and keeps it up for the clients that join as
 * secondaries. xal reads filesystem metadata over the same device.
 *
 * @param device	Device whose ->uri is opened into ->dev.
 * @param shm_id	Multi-process group to own; 0 opens the device unshared.
 * @return			0 on success, negative errno on failure.
 */
int
homid_xnvme_setup(struct homid_device *device, uint32_t shm_id);

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
 * filesystem only appears once a client joins the group and serves it, so it
 * cannot exist at daemon startup), then indexes each device's xal over that
 * mount and publishes the shm. Call after the controller is up, so qublk can
 * join the group and create the mount being waited on.
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
