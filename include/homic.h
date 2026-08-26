#ifndef HOMIC_H
#define HOMIC_H

#include <limits.h>
#include <stdint.h>

#include <libxal.h>

#include <homi_proto.h>

/**
 * One file-to-LBA extent.
 *
 * file_offset and length are byte quantities; slba is the starting logical
 * block address on the device. homic_get_extents returns an array of these.
 */
struct homic_extent {
	uint64_t file_offset; ///< Byte offset within the file
	uint64_t slba;        ///< Starting LBA on the device
	uint64_t length;      ///< Extent length in bytes
};

/**
 * One device the daemon serves.
 *
 * mountpoint is the filesystem that device holds, which is what tells a caller
 * which device backs an open file. It is empty when the daemon indexes the
 * device without a kernel mount.
 */
struct homic_device {
	char dev_uri[HOMID_DEVURI_MAXLEN];
	char mountpoint[PATH_MAX];
};

/**
 * Connect to the homid daemon.
 *
 * Opens a Unix domain socket connection to the daemon. Must be called before
 * any other homic functions. The connection is held globally; call
 * homic_disconnect() to release it.
 *
 * @return  0 on success, negative errno on failure.
 */
int
homic_connect(char *socket_path);

/**
 * Disconnect from the homid daemon.
 *
 * Closes the socket and releases the global connection. Safe to call if not
 * connected.
 */
void
homic_disconnect();

/**
 * List the devices the daemon serves.
 *
 * Returns the daemon's device set, so a caller configures nothing of its own.
 * Requires an active connection established with homic_connect().
 *
 * @param out  Output: heap-allocated device array (caller frees).
 * @param n    Output: number of devices.
 * @return     0 on success, negative errno on failure.
 */
int
homic_list_devices(struct homic_device **out, uint32_t *n);

/**
 * Connect to xal for a specific device.
 *
 * Sends an XAL_CONNECT request to the daemon, maps the inode and extent pools
 * from POSIX shared memory, and constructs a read-only xal via xal_from_pools().
 * Requires an active connection established with homic_connect().
 *
 * @param dev_uri  URI of the device to connect to.
 * @param out      Output: read-only xal struct backed by shared memory.
 * @return         0 on success, negative errno on failure.
 */
int
homic_connect_xal(char *dev_uri, struct xal **out);

/**
 * Flag a device's xal dirty.
 *
 * Asks the daemon to set the dirty flag immediately, so a caller that just
 * changed the filesystem (e.g. an allocating write) guarantees the next extent
 * resolution sees it as stale (-ESTALE). The daemon re-indexes on its own (its
 * watch thread), so the caller need only retry the resolve; no client-driven
 * re-index is required. Requires an active connection established with
 * homic_connect().
 *
 * @param dev_uri  URI of the device whose xal to flag dirty.
 * @return         0 on success, negative errno on failure.
 */
int
homic_mark_dirty(char *dev_uri);

/**
 * Resolve an open file's extents to device LBAs via FIEMAP.
 *
 * FIEMAPs the kernel filesystem mounted over the qublk device and converts each
 * mapped extent into a device extent, returned as a heap-allocated array via
 * *out (caller frees) with the count in *n. Physical byte offsets are divided by
 * the backing block device's logical block size to yield the starting LBA, which
 * is the NVMe LBA since qublk maps the namespace one-to-one.
 *
 * The caller supplies an fd opened on the mount; no daemon round-trip is needed.
 *
 * @param dev_uri  URI of the device backing the filesystem.
 * @param fd   File descriptor opened on the qublk-mounted filesystem.
 * @param out  Output: heap-allocated extent array (caller frees).
 * @param n    Output: number of extents.
 * @return     0 on success, negative errno on failure.
 */
int
homic_get_extents(char *dev_uri, int fd, struct homic_extent **out, uint32_t *n);

#endif /* HOMIC_H */
