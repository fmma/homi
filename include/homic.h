#ifndef HOMIC_H
#define HOMIC_H

#include <libxal.h>

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
 * Wait until the xal pools are not being reindexed.
 *
 * Spins while the daemon is running xal_index(). Returns once it is safe to
 * read from the xal pools. Requires an active connection established with
 * homic_connect().
 *
 * @param xal  xal instance to wait on.
 * @return     0 on success, negative errno on failure.
 */
int
homic_xal_wait(struct xal *xal);

/**
 * Request a slice of a device's I/O qpair pool from the daemon.
 *
 * Asks homid for `nqpairs` I/O qpairs on `dev_uri`, writes the returned attach
 * descriptor to a file, and returns its path via *out_descpath (caller frees).
 * Set XNVME_UPCIE_ATTACH to that path, then xnvme_dev_open(dev_uri, be="upcie")
 * to drive the handed-out qpairs without owning the controller.
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
 * Tells the daemon the attached I/O qpairs are no longer in use so a later
 * attach (this process or another) can reuse them. Call after closing the
 * xNVMe device that drove them. homic_disconnect() also does this if the
 * client forgot. No-op if nothing is currently attached.
 *
 * @return 0 on success, negative errno on failure.
 */
int
homic_detach_qpair(void);

#endif /* HOMIC_H */
