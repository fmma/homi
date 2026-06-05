#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <libxal.h>
#include <libxnvme.h>

#include <homid.h>
#include <homid_log.h>
#include <homid_qpair.h>
#include <homid_xal.h>
#include <homid_opts.h>

#define HOMID_NSID 1
#define HOMID_POOL_SIZE 16
#define HOMID_QPAIR_DEPTH 1024

static void
on_xal_dirty(struct xal *xal, void *cb_args)
{
	int err;

	(void)cb_args;

	err = xal_index(xal);
	if (err) {
		homid_log(LOG_CRIT, "xal_index(): %d; pools are stale, daemon restart required", err);
	}
}

int
homid_xal_setup(struct xal_opts *opts, struct homid_device *device)
{
	struct xal *xal;
	int err;

	if (!device) {
		err = -EINVAL;
		homid_log(LOG_ERR, "No homid_device for xal setup: %d", err);
		return err;
	}

	err = xal_open(device->dev, &xal, opts);
	if (err) {
		homid_log(LOG_ERR, "xal_open(): %d", err);
		return err;
	}

	err = xal_dinodes_retrieve(xal);
	if (err) {
		homid_log(LOG_ERR, "xal_dinodes_retrieve(): %d", err);
		goto close_xal;
	}

	err = xal_index(xal);
	if (err) {
		homid_log(LOG_ERR, "xal_index(): %d", err);
		goto close_xal;
	}

	device->watching = false;
	if (opts->watch_mode) {
		err = xal_watch_filesystem(xal, on_xal_dirty, NULL);
		if (err) {
			homid_log(LOG_WARNING, "xal_watch_filesystem(): %d; filesystem watch unavailable", err);
		} else {
			device->watching = true;
		}
	}

	device->xal = xal;

	return 0;

close_xal:
	xal_close(xal);
	return err;
}

/**
 * Point device->dev at the owner-mode controller for xal.
 *
 * The qpair owner already opened the controller in owner mode (device->qpo) and
 * holds its admin queue and a sync I/O qpair. xal reads the on-disk filesystem
 * metadata over that dev. The dev belongs to the qpair owner and is closed with
 * it.
 */
int
homid_xnvme_setup(struct homid_device *device)
{
	device->dev = homid_qpair_owner_dev(device->qpo);
	if (!device->dev) {
		homid_log(LOG_ERR, "no owner dev for %s", device->uri);
		return -EINVAL;
	}

	return 0;
}

void
homid_device_close(unsigned int ndevs, struct homid_device *devices)
{
	if (!devices) {
		return;
	}

	for (unsigned int i = 0; i < ndevs; i++) {
		struct homid_device *dev = &devices[i];

		if (!dev) {
			continue;
		}

		if (dev->watching) {
			xal_stop_watching_filesystem(dev->xal);
		}

		if (dev->xal) {
			xal_close(dev->xal);
		}

		/* dev->dev aliases the qpair owner's controller dev; closing the
		 * owner (below) closes it, so do not close it here. xal_close
		 * above still ran while the dev was open. */
		if (dev->qpo) {
			homid_qpair_owner_close(dev->qpo);
			free(dev->qpo);
		}
	}

	free(devices);
}

int
homid_device_setup(struct homid_opts *opts, struct homid_device **devices)
{
	struct xal_opts *xal_opts = &opts->xal_opts;
	struct homid_device *devs;
	unsigned int ndevs = opts->ndevs;
	int err;

	devs = calloc(ndevs, sizeof(struct homid_device));
	if (!devs) {
		err = -errno;
		homid_log(LOG_ERR, "Failed to allocate devices: %d", err);
		return err;
	}

	for (unsigned int i = 0; i < ndevs; i++) {
		char *uri = opts->dev_uris[i];

		strncpy(devs[i].uri, uri, sizeof(devs[i].uri) - 1);
		snprintf(devs[i].shm_name, sizeof(devs[i].shm_name), "/homid_dev%u", i);
		xal_opts->shm_name = devs[i].shm_name;

		devs[i].qpo = calloc(1, sizeof(*devs[i].qpo));
		if (!devs[i].qpo) {
			err = -ENOMEM;
			goto failed;
		}

		err = homid_qpair_owner_open(devs[i].qpo, uri, HOMID_NSID, HOMID_POOL_SIZE,
					     HOMID_QPAIR_DEPTH);
		if (err) {
			homid_log(LOG_ERR, "Failed to own controller for %s: %d", uri, err);
			goto failed;
		}

		err = homid_xnvme_setup(&devs[i]);
		if (err) {
			homid_log(LOG_ERR, "Failed to setup xNVMe for %s: %d", uri, err);
			goto failed;
		}

		/* xal indexing needs a valid on-device filesystem; when none is
		 * present yet (e.g. before mkfs over the ublk device) keep serving
		 * qpairs without xal. */
		err = homid_xal_setup(xal_opts, &devs[i]);
		if (err) {
			homid_log(LOG_WARNING,
				  "XAL setup for %s failed (%d); serving qpairs without xal",
				  uri, err);
			devs[i].xal = NULL;
		}
	}

	*devices = devs;
	return 0;

failed:
	homid_device_close(ndevs, devs);
	return err;
}

struct homid_device *
homid_device_get(struct homid *homid, char *uri)
{
	struct homid_device *found = NULL;

	for (unsigned int i = 0; i < homid->ndevs; i++) {
		if (!strcmp(homid->dev[i].uri, uri)) {
			found = &homid->dev[i];
			break;
		}
	}

	return found;
}
