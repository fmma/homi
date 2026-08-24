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
#include <homid_xal.h>
#include <homid_opts.h>

/* The daemon drives no I/O of its own: xal reads go over the sync path and the
 * admin queue serves the multi-process group. Keep its private DMA heap small so
 * the hugepages left over go to the clients that do the reading. */
#define HOMID_HOST_HEAP_NBYTES (16UL << 20)

/* Watch-thread callback: the xal watcher invokes this when the filesystem goes
 * dirty (a breaking change or a client mark-dirty), so the daemon re-indexes
 * itself rather than waiting for a client request. Runs on the watch thread. */
static void
on_xal_dirty(struct xal *xal, void *cb_args)
{
	(void)xal;
	homid_xal_reindex((struct homid_device *)cb_args);
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

	/* Publish the xal before starting the watch so the dirty callback (which
	 * re-indexes via device->xal) always sees a ready device. */
	device->xal = xal;

	/* Daemon-initiated re-indexing: the watcher flags the xal dirty on a
	 * filesystem change and calls on_xal_dirty on its own thread to rebuild
	 * the index in place. The single watch thread serializes the rewrite and
	 * the seqlock lets cross-process readers detect it (homic_get_extents
	 * returns -ESTALE), so no client request and no quiescing is needed. */
	device->watching = false;
	if (opts->watch_mode) {
		err = xal_watch_filesystem(xal, on_xal_dirty, device);
		if (err) {
			homid_log(LOG_WARNING, "xal_watch_filesystem(): %d; filesystem watch unavailable", err);
		} else {
			device->watching = true;
		}
	}

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

	/* xal_index rewrites the shared pools in place; serialize so two re-index
	 * passes cannot run it concurrently on the same xal. Readers detect the
	 * rewrite via the seqlock (xal_get_seq_lock), so no quiescing is needed. */
	pthread_mutex_lock(&lock);
	err = xal_index(device->xal);
	pthread_mutex_unlock(&lock);

	if (err) {
		homid_log(LOG_ERR, "homid_xal_reindex(): %d", err);
	}

	return err;
}

/**
 * Open the device into the shared multi-process group.
 *
 * The daemon opens first, so it wins xNVMe's role election and becomes the
 * primary: it brings the controller up and holds it up for as long as it runs.
 * Clients open the same device with the same shm_id and join as secondaries,
 * each allocating its own I/O queues from the shared queue-id map.
 */
int
homid_xnvme_setup(struct homid_device *device, uint32_t shm_id)
{
	struct xnvme_opts opts = xnvme_opts_default();

	opts.be = "upcie";
	opts.shm_id = shm_id;
	opts.host_heap_size = HOMID_HOST_HEAP_NBYTES;

	device->dev = xnvme_dev_open(device->uri, &opts);
	if (!device->dev) {
		homid_log(LOG_ERR, "xnvme_dev_open(%s, shm_id=%u) failed", device->uri, shm_id);
		return -ENODEV;
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

		if (dev->dev) {
			xnvme_dev_close(dev->dev);
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

		err = homid_xnvme_setup(&devs[i], opts->shm_id);
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
