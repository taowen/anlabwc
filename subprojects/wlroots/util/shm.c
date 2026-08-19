#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wlr/config.h>
#include "util/shm.h"

#ifdef __ANDROID__
#include <linux/memfd.h>
#include <sys/syscall.h>

static int android_memfd(const char *name) {
	return (int)syscall(__NR_memfd_create, name, MFD_CLOEXEC);
}

static int ftruncate_size(int fd, size_t size) {
	int ret;
	do {
		ret = ftruncate(fd, (off_t)size);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

int allocate_shm_file(size_t size) {
	int fd = android_memfd("wlroots-shm");
	if (fd < 0) {
		return -1;
	}
	if (ftruncate_size(fd, size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

bool allocate_shm_file_pair(size_t size, int *rw_fd_ptr, int *ro_fd_ptr) {
	int rw_fd = android_memfd("wlroots-shm");
	if (rw_fd < 0) {
		return false;
	}
	if (ftruncate_size(rw_fd, size) < 0) {
		close(rw_fd);
		return false;
	}
	int ro_fd = fcntl(rw_fd, F_DUPFD_CLOEXEC, 0);
	if (ro_fd < 0) {
		close(rw_fd);
		return false;
	}
	*rw_fd_ptr = rw_fd;
	*ro_fd_ptr = ro_fd;
	return true;
}

#else

#define RANDNAME_PATTERN "/wlroots-XXXXXX"

static void randname(char *buf) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

static int excl_shm_open(char *name) {
	int retries = 100;
	do {
		randname(name + strlen(RANDNAME_PATTERN) - 6);

		--retries;
		// CLOEXEC is guaranteed to be set by shm_open
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

int allocate_shm_file(size_t size) {
	char name[] = RANDNAME_PATTERN;
	int fd = excl_shm_open(name);
	if (fd < 0) {
		return -1;
	}
	shm_unlink(name);

	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

bool allocate_shm_file_pair(size_t size, int *rw_fd_ptr, int *ro_fd_ptr) {
	char name[] = RANDNAME_PATTERN;
	int rw_fd = excl_shm_open(name);
	if (rw_fd < 0) {
		return false;
	}

	// CLOEXEC is guaranteed to be set by shm_open
	int ro_fd = shm_open(name, O_RDONLY, 0);
	if (ro_fd < 0) {
		shm_unlink(name);
		close(rw_fd);
		return false;
	}

	shm_unlink(name);

	// Make sure the file cannot be re-opened in read-write mode (e.g. via
	// "/proc/self/fd/" on Linux)
	if (fchmod(rw_fd, 0) != 0) {
		close(rw_fd);
		close(ro_fd);
		return false;
	}

	int ret;
	do {
		ret = ftruncate(rw_fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(rw_fd);
		close(ro_fd);
		return false;
	}

	*rw_fd_ptr = rw_fd;
	*ro_fd_ptr = ro_fd;
	return true;
}

#endif
