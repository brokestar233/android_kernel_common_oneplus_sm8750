// SPDX-License-Identifier: GPL-2.0
/*
 * The main purpose of the tests here is to exercise the migration entry code
 * paths in the kernel.
 */

#include "../kselftest_harness.h"
#include <strings.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>

#include "vm_util.h"

#define TWOMEG (2<<20)
#define RUNTIME (20)
#define KSM_RUN_PATH "/sys/kernel/mm/ksm/run"
#define KSM_FULL_SCANS_PATH "/sys/kernel/mm/ksm/full_scans"

#define ALIGN(x, a) (((x) + (a - 1)) & (~((a) - 1)))

static long read_sysfs_long(const char *path)
{
	char buf[32];
	ssize_t ret;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	ret = pread(fd, buf, sizeof(buf) - 1, 0);
	close(fd);
	if (ret <= 0)
		return -errno;

	buf[ret] = '\0';
	return strtol(buf, NULL, 10);
}

static int write_sysfs_string(const char *path, const char *value)
{
	ssize_t len = strlen(value);
	ssize_t ret;
	int fd;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;

	ret = write(fd, value, len);
	close(fd);
	if (ret != len)
		return -errno;

	return 0;
}

static int wait_ksm_full_scans(unsigned int scans)
{
	long start_scans, cur_scans;
	int retries = 500;

	start_scans = read_sysfs_long(KSM_FULL_SCANS_PATH);
	if (start_scans < 0)
		return start_scans;

	if (write_sysfs_string(KSM_RUN_PATH, "1") < 0)
		return -errno;

	do {
		usleep(10000);
		cur_scans = read_sysfs_long(KSM_FULL_SCANS_PATH);
		if (cur_scans < 0)
			return cur_scans;
	} while (cur_scans < start_scans + scans && --retries > 0);

	return retries > 0 ? 0 : -ETIMEDOUT;
}

FIXTURE(migration)
{
	pthread_t *threads;
	pid_t *pids;
	int nthreads;
	int n1;
	int n2;
};

FIXTURE_SETUP(migration)
{
	int n;

	ASSERT_EQ(numa_available(), 0);
	self->nthreads = numa_num_task_cpus() - 1;
	self->n1 = -1;
	self->n2 = -1;

	for (n = 0; n < numa_max_possible_node(); n++)
		if (numa_bitmask_isbitset(numa_all_nodes_ptr, n)) {
			if (self->n1 == -1) {
				self->n1 = n;
			} else {
				self->n2 = n;
				break;
			}
		}

	self->threads = malloc(self->nthreads * sizeof(*self->threads));
	ASSERT_NE(self->threads, NULL);
	self->pids = malloc(self->nthreads * sizeof(*self->pids));
	ASSERT_NE(self->pids, NULL);
};

FIXTURE_TEARDOWN(migration)
{
	free(self->threads);
	free(self->pids);
}

int migrate(uint64_t *ptr, int n1, int n2)
{
	int ret, tmp;
	int status = 0;
	struct timespec ts1, ts2;

	if (clock_gettime(CLOCK_MONOTONIC, &ts1))
		return -1;

	while (1) {
		if (clock_gettime(CLOCK_MONOTONIC, &ts2))
			return -1;

		if (ts2.tv_sec - ts1.tv_sec >= RUNTIME)
			return 0;

		ret = move_pages(0, 1, (void **) &ptr, &n2, &status,
				MPOL_MF_MOVE_ALL);
		if (ret) {
			if (ret > 0)
				printf("Didn't migrate %d pages\n", ret);
			else
				perror("Couldn't migrate pages");
			return -2;
		}

		tmp = n2;
		n2 = n1;
		n1 = tmp;
	}

	return 0;
}

void *access_mem(void *ptr)
{
	volatile uint64_t y = 0;
	volatile uint64_t *x = ptr;

	while (1) {
		pthread_testcancel();
		y += *x;

		/* Prevent the compiler from optimizing out the writes to y: */
		asm volatile("" : "+r" (y));
	}

	return NULL;
}

/*
 * Basic migration entry testing. One thread will move pages back and forth
 * between nodes whilst other threads try and access them triggering the
 * migration entry wait paths in the kernel.
 */
TEST_F_TIMEOUT(migration, private_anon, 2*RUNTIME)
{
	uint64_t *ptr;
	int i;

	if (self->nthreads < 2 || self->n1 < 0 || self->n2 < 0)
		SKIP(return, "Not enough threads or NUMA nodes available");

	ptr = mmap(NULL, TWOMEG, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	memset(ptr, 0xde, TWOMEG);
	for (i = 0; i < self->nthreads - 1; i++)
		if (pthread_create(&self->threads[i], NULL, access_mem, ptr))
			perror("Couldn't create thread");

	ASSERT_EQ(migrate(ptr, self->n1, self->n2), 0);
	for (i = 0; i < self->nthreads - 1; i++)
		ASSERT_EQ(pthread_cancel(self->threads[i]), 0);
}

/*
 * Same as the previous test but with shared memory.
 */
TEST_F_TIMEOUT(migration, shared_anon, 2*RUNTIME)
{
	pid_t pid;
	uint64_t *ptr;
	int i;

	if (self->nthreads < 2 || self->n1 < 0 || self->n2 < 0)
		SKIP(return, "Not enough threads or NUMA nodes available");

	ptr = mmap(NULL, TWOMEG, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	memset(ptr, 0xde, TWOMEG);
	for (i = 0; i < self->nthreads - 1; i++) {
		pid = fork();
		if (!pid) {
			prctl(PR_SET_PDEATHSIG, SIGHUP);
			/* Parent may have died before prctl so check now. */
			if (getppid() == 1)
				kill(getpid(), SIGHUP);
			access_mem(ptr);
		} else {
			self->pids[i] = pid;
		}
	}

	ASSERT_EQ(migrate(ptr, self->n1, self->n2), 0);
	for (i = 0; i < self->nthreads - 1; i++)
		ASSERT_EQ(kill(self->pids[i], SIGTERM), 0);
}

/*
 * Tests the pmd migration entry paths.
 */
TEST_F_TIMEOUT(migration, private_anon_thp, 2*RUNTIME)
{
	uint64_t *ptr;
	int i;

	if (self->nthreads < 2 || self->n1 < 0 || self->n2 < 0)
		SKIP(return, "Not enough threads or NUMA nodes available");

	ptr = mmap(NULL, 2*TWOMEG, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(ptr, MAP_FAILED);

	ptr = (uint64_t *) ALIGN((uintptr_t) ptr, TWOMEG);
	ASSERT_EQ(madvise(ptr, TWOMEG, MADV_HUGEPAGE), 0);
	memset(ptr, 0xde, TWOMEG);
	for (i = 0; i < self->nthreads - 1; i++)
		if (pthread_create(&self->threads[i], NULL, access_mem, ptr))
			perror("Couldn't create thread");

	ASSERT_EQ(migrate(ptr, self->n1, self->n2), 0);
	for (i = 0; i < self->nthreads - 1; i++)
		ASSERT_EQ(pthread_cancel(self->threads[i]), 0);
}

TEST_F_TIMEOUT(migration, ksm_and_mremap, 2*RUNTIME)
{
	char saved_run[32] = { 0 };
	char *region, *peer;
	unsigned long page_sz;
	unsigned long region_pfn, peer_pfn;
	ssize_t saved_run_len;
	int pagemap_fd, run_fd;
	int status = 0;
	int ret;

	if (self->n1 < 0 || self->n2 < 0)
		SKIP(return, "Not enough NUMA nodes available");

	run_fd = open(KSM_RUN_PATH, O_RDWR);
	if (run_fd < 0)
		SKIP(return, "KSM sysfs is unavailable");

	saved_run_len = pread(run_fd, saved_run, sizeof(saved_run) - 1, 0);
	if (saved_run_len < 0)
		SKIP(return, "Cannot read current KSM run state");

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	ASSERT_GE(pagemap_fd, 0);

	page_sz = getpagesize();
	region = mmap(NULL, 2 * page_sz, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(region, MAP_FAILED);
	peer = mmap(NULL, page_sz, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(peer, MAP_FAILED);

	memset(region, 0x77, 2 * page_sz);
	memset(peer, 0x77, page_sz);

	region = mremap(region + page_sz, page_sz, page_sz,
			MREMAP_MAYMOVE | MREMAP_FIXED, region);
	ASSERT_NE(region, MAP_FAILED);

	ASSERT_EQ(madvise(region, page_sz, MADV_MERGEABLE), 0);
	ASSERT_EQ(madvise(peer, page_sz, MADV_MERGEABLE), 0);

	ret = wait_ksm_full_scans(2);
	ASSERT_EQ(ret, 0);

	region_pfn = pagemap_get_pfn(pagemap_fd, region);
	peer_pfn = pagemap_get_pfn(pagemap_fd, peer);
	ASSERT_NE(region_pfn, -1ul);
	ASSERT_NE(peer_pfn, -1ul);
	ASSERT_EQ(region_pfn, peer_pfn);

	ret = move_pages(0, 1, (void **)&region, &self->n2, &status,
			 MPOL_MF_MOVE_ALL);
	ASSERT_EQ(ret, 0);
	ASSERT_EQ(status, 0);

	ASSERT_EQ(pwrite(run_fd, saved_run, saved_run_len, 0), saved_run_len);
	ASSERT_EQ(close(run_fd), 0);
	ASSERT_EQ(close(pagemap_fd), 0);
	ASSERT_EQ(munmap(peer, page_sz), 0);
	ASSERT_EQ(munmap(region, page_sz), 0);
}

TEST_HARNESS_MAIN
