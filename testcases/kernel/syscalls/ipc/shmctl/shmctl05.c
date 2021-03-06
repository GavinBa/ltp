/*
 * Copyright (c) 2018 Google, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program, if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Regression test for commit 3f05317d9889 ("ipc/shm: fix use-after-free of shm
 * file via remap_file_pages()").  This bug allowed the remap_file_pages()
 * syscall to use the file of a System V shared memory segment after its ID had
 * been reallocated and the file freed.  This test reproduces the bug as a NULL
 * pointer dereference in touch_atime(), although it's a race condition so it's
 * not guaranteed to work.  This test is based on the reproducer provided in the
 * fix's commit message.
 */

#include "lapi/syscalls.h"
#include "tst_test.h"
#include "tst_fuzzy_sync.h"
#include "tst_safe_pthread.h"
#include "tst_safe_sysv_ipc.h"
#include "tst_timer.h"

static struct tst_fzsync_pair fzsync_pair = TST_FZSYNC_PAIR_INIT;

static pthread_t thrd;

/*
 * Thread 2: repeatedly remove the shm ID and reallocate it again for a
 * new shm segment.
 */
static void *thrproc(void *unused)
{
	int id = SAFE_SHMGET(0xF00F, 4096, IPC_CREAT|0700);

	for (;;) {
		if (!tst_fzsync_wait_b(&fzsync_pair))
			break;
		SAFE_SHMCTL(id, IPC_RMID, NULL);
		id = SAFE_SHMGET(0xF00F, 4096, IPC_CREAT|0700);
		if (!tst_fzsync_wait_b(&fzsync_pair))
			break;
	}
	return unused;
}

static void setup(void)
{
	tst_timer_check(CLOCK_MONOTONIC);

	/* Skip test if either remap_file_pages() or SysV IPC is unavailable */
	tst_syscall(__NR_remap_file_pages, NULL, 0, 0, 0, 0);
	tst_syscall(__NR_shmctl, 0xF00F, IPC_RMID, NULL);

	SAFE_PTHREAD_CREATE(&thrd, NULL, thrproc, NULL);
}

static void do_test(void)
{
	tst_timer_start(CLOCK_MONOTONIC);

	/*
	 * Thread 1: repeatedly attach a shm segment, then remap it until the ID
	 * seems to have been removed by the other process.
	 */
	while (!tst_timer_expired_ms(5000)) {
		int id;
		void *addr;

		id = SAFE_SHMGET(0xF00F, 4096, IPC_CREAT|0700);
		addr = SAFE_SHMAT(id, NULL, 0);
		tst_fzsync_wait_a(&fzsync_pair);
		do {
			/* This is the system call that crashed */
			TEST(syscall(__NR_remap_file_pages, addr, 4096,
				     0, 0, 0));
		} while (TEST_RETURN == 0);

		if (TEST_ERRNO != EIDRM && TEST_ERRNO != EINVAL) {
			tst_brk(TBROK | TTERRNO,
				"Unexpected remap_file_pages() error");
		}
		tst_fzsync_wait_a(&fzsync_pair);
	}

	tst_res(TPASS, "didn't crash");
}

static void cleanup(void)
{
	if (thrd) {
		tst_fzsync_pair_exit(&fzsync_pair);
		SAFE_PTHREAD_JOIN(thrd, NULL);
	}
	shmctl(0xF00F, IPC_RMID, NULL);
}

static struct tst_test test = {
	.timeout = 20,
	.setup = setup,
	.test_all = do_test,
	.cleanup = cleanup,
};
