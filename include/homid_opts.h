#ifndef HOMID_OPTS_H
#define HOMID_OPTS_H

#include <stdint.h>

#include <libxal.h>
#include <homi_proto.h>

struct homid_opts {
	int log_level;
	unsigned int ndevs;
	char (*dev_uris)[HOMID_DEVURI_MAXLEN];
	char *ipc_socket;

	/* xNVMe multi-process group the daemon and its clients share. The daemon
	 * starts first, so it wins the role election and becomes the primary that
	 * owns the controller; clients join as secondaries. Zero disables sharing,
	 * which leaves the daemon the only user of the device. */
	uint32_t shm_id;

	struct xal_opts xal_opts;
};

/**
 * Parse the TOML configuration file
 *
 * We expect the configuration file to have keys:
 * - log_level (int)
 * - devices (array of strings)
 * - ipc_socket (string)
 * - shm_id (int, optional)
 * - xal.backend (int)
 * - xal.watchmode (int)
 * - xal.file_lookupmode (int)
 *
 * @param path Path to the configuration file
 * @param opts homid_opts struct that the configuration will be loaded into
 */
int
homid_opts_from_toml(char *path, struct homid_opts *opts);

#endif /* HOMID_OPTS_H */
