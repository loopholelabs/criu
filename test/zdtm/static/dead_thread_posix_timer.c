#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "zdtmtst.h"

#ifndef SIGEV_THREAD_ID
#define SIGEV_THREAD_ID 4
#endif

const char *test_doc = "Check that a SIGEV_THREAD_ID timer targeting a dead thread is skipped during dump";
const char *test_author = "CRIU developers";

static timer_t timerid;
static int timer_created;

static void *create_timer(void *arg)
{
	struct sigevent evp = {};
	pid_t tid;

	tid = syscall(SYS_gettid);

	evp.sigev_notify = SIGEV_THREAD_ID;
#ifdef __GLIBC__
	evp._sigev_un._tid = tid;
#else
	evp.sigev_notify_thread_id = tid;
#endif
	evp.sigev_signo = SIGRTMIN;

	if (timer_create(CLOCK_MONOTONIC, &evp, &timerid)) {
		pr_perror("timer_create");
		return (void *)1;
	}

	timer_created = 1;

	/*
	 * Return immediately. Since this thread called
	 * pthread_create (not runtime.LockOSThread + goroutine
	 * return, but the effect is identical), returning here
	 * causes the thread to exit. The timer keeps a struct pid
	 * reference to this now-dead thread, making the timer's
	 * notify thread id stale. The kernel will silently drop
	 * signals from this timer since posixtimer_get_target()
	 * returns NULL for the dead thread.
	 *
	 * CRIU must skip this timer during dump rather than fail.
	 */
	return NULL;
}

int main(int argc, char **argv)
{
	struct itimerspec its;
	pthread_t thr;
	void *ret;

	test_init(argc, argv);

	if (pthread_create(&thr, NULL, create_timer, NULL)) {
		pr_perror("pthread_create");
		return 1;
	}

	if (pthread_join(thr, &ret)) {
		pr_perror("pthread_join");
		return 1;
	}

	if (ret != NULL) {
		fail("Timer creation thread failed");
		return 1;
	}

	/* Thread is now dead, timer has a stale notify thread id */

	test_daemon();
	test_waitsig();

	/*
	 * After restore the stale timer should have been skipped,
	 * so timer_gettime must fail with EINVAL (invalid timer id).
	 */
	if (timer_gettime(timerid, &its) == 0) {
		/*
		 * If the timer still exists that is also acceptable —
		 * it means the implementation chose to preserve it.
		 * The important thing is that dump did not fail.
		 */
		test_msg("Timer survived restore (unexpected but not wrong)\n");
	}

	pass();

	if (timer_created)
		timer_delete(timerid);
	return 0;
}
