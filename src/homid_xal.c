#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
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

	/* Watch only to flag the xal dirty on filesystem changes; re-indexing is
	 * explicit (HOMI_MSG_TYPE_XAL_REINDEX) so the shared pools are never
	 * rewritten under a reader. A dirty xal is reported to clients, which
	 * trigger a re-index when the filesystem is quiescent. */
	device->watching = false;
	if (opts->watch_mode) {
		err = xal_watch_filesystem(xal, NULL, NULL);
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

int
homid_xal_reindex(struct homid_device *device)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int err;

	if (!device || !device->xal) {
		return -EAGAIN;
	}

	/* xal_index rewrites the shared pools in place; serialize so two reindex
	 * requests cannot run it concurrently on the same xal. Callers keep the
	 * filesystem quiescent across the call. */
	pthread_mutex_lock(&lock);
	err = xal_index(device->xal);
	pthread_mutex_unlock(&lock);

	if (err) {
		homid_log(LOG_ERR, "homid_xal_reindex(): %d", err);
	}

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

extern volatile sig_atomic_t stop;

static pthread_t indexer_thread;
static bool indexer_started;
static struct homid *indexer_homid;
static struct xal_opts *indexer_opts;

static bool
is_mounted(const char *path)
{
	struct stat st, parent;
	char dir[PATH_MAX];

	if (snprintf(dir, sizeof(dir), "%s/..", path) >= (int)sizeof(dir)) {
		return false;
	}
	if (stat(path, &st) != 0 || stat(dir, &parent) != 0) {
		return false;
	}

	return st.st_dev != parent.st_dev;
}

static void *
homid_xal_index_loop(void *arg)
{
	struct homid *homid = indexer_homid;
	struct xal_opts *opts = indexer_opts;
	const char *mnt = opts->mountpoint;

	(void)arg;

	for (unsigned int i = 0; i < homid->ndevs && !stop; i++) {
		struct homid_device *dev = &homid->dev[i];

		if (mnt && mnt[0]) {
			while (!stop && !is_mounted(mnt)) {
				usleep(200000);
			}
		}
		if (stop) {
			break;
		}

		opts->shm_name = dev->shm_name;
		if (homid_xal_setup(opts, dev)) {
			homid_log(LOG_ERR, "deferred xal index failed for %s", dev->uri);
			continue;
		}

		char ready[PATH_MAX];
		int rfd;

		snprintf(ready, sizeof(ready), "/dev/shm%s.ready", dev->shm_name);
		rfd = open(ready, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (rfd >= 0) {
			close(rfd);
		}

		homid_log(LOG_NOTICE, "xal indexed for %s%s%s", dev->uri,
			  (mnt && mnt[0]) ? " at " : "",
			  (mnt && mnt[0]) ? mnt : "");
	}

	return NULL;
}

void
homid_xal_index_start(struct homid *homid, struct xal_opts *opts)
{
	indexer_homid = homid;
	indexer_opts = opts;

	if (pthread_create(&indexer_thread, NULL, homid_xal_index_loop, NULL) != 0) {
		homid_log(LOG_ERR, "Failed to start xal indexer thread");
		return;
	}
	indexer_started = true;
}

void
homid_xal_index_stop(void)
{
	if (indexer_started) {
		pthread_join(indexer_thread, NULL);
		indexer_started = false;
	}
}
