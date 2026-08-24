#ifndef HOMIC_H
#define HOMIC_H

#include <stdint.h>

#include <libxal.h>

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
 * Request a slice of a device's I/O qpair pool from the daemon.
 *
 * Asks homid for `nqpairs` I/O qpairs on `dev_uri`, writes the returned attach
 * descriptor to a file, and returns its path via *out_descpath (caller frees).
 * Set XNVME_UPCIE_ATTACH to that path, then xnvme_dev_open(dev_uri, be="upcie")
 * to drive the handed-out qpairs without owning the controller.
 *
 * The attach connection is held open for the lifetime of the qpairs: the daemon
 * reclaims them when it closes, whether via homic_detach_qpair() or because the
 * client exits, so a crashing client cannot leak qpairs.
 *
 * @param dev_uri       Device URI as configured in the daemon.
 * @param nqpairs       Number of I/O qpairs to request (0 means 1).
 * @param out_descpath  Output: path to the attach descriptor (caller frees).
 * @return              0 on success, negative errno on failure.
 */
int
homic_attach_qpair(char *dev_uri, unsigned nqpairs, char **out_descpath);

/**
 * Return the qpairs from the most recent homic_attach_qpair() to the pool.
 *
 * Closes the held attach connection, which tells the daemon to reclaim the
 * qpairs so a later attach (this process or another) can reuse them. Call after
 * closing the xNVMe device that drove them. homic_disconnect() also does this if
 * the client forgot. No-op if nothing is currently attached.
 *
 * @return 0 on success, negative errno on failure.
 */
int
homic_detach_qpair(void);

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
 * @param fd   File descriptor opened on the qublk-mounted filesystem.
 * @param out  Output: heap-allocated extent array (caller frees).
 * @param n    Output: number of extents.
 * @return     0 on success, negative errno on failure.
 */
int
homic_get_extents(int fd, struct homic_extent **out, uint32_t *n);

#endif /* HOMIC_H */
