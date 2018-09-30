#include "wire_io.h"

#include "wire.h"
#include "wire_fd.h"
#include "wire_stack.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <assert.h>
#include <stdarg.h>

struct wire_io {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct list_head list;
	wire_fd_state_t fd_state;
	int response_send_fd;
	int response_recv_fd;
	int num_active_ios;
	wire_t wire;
};

static __thread struct wire_io wire_io;
static __thread bool is_wire_thread;

struct wire_io_act_common {
	struct list_head elem;
	wire_wait_t *wait;
};

/* This runs in the wire thread and should hopefully only see rare lock contention and as such not really block at all.
 */
static void submit_action(struct wire_io_act_common *act)
{
	wire_wait_list_t wait_list;
	wire_wait_t wait_item;

	wire_wait_list_init(&wait_list);
	wire_wait_init(&wait_item);
	wire_wait_chain(&wait_list, &wait_item);

	act->wait = &wait_item;

	// Add the action to the list
	pthread_mutex_lock(&wire_io.mutex);
	list_add_tail(&act->elem, &wire_io.list);
	pthread_mutex_unlock(&wire_io.mutex);

	// Wake at least one worker thread to get this action done
	pthread_cond_signal(&wire_io.cond);

	// Wake the reply waiting wire if needed
	if (wire_io.num_active_ios == 0) {
		wire_resume(&wire_io.wire);
	}

	// Wait for the action to complete
	wire_io.num_active_ios++;
	wire_fd_mode_read(&wire_io.fd_state);
	wire_list_wait(&wait_list);
}

static int (*orig_ioctl)(int fd, unsigned long request, ...);
static int gen_ioctl(int fd, unsigned long request, void* argp);
int ioctl(int fd, unsigned long request, ...)
{
	void* argp;

	va_list arg;
	va_start(arg, request);
	argp = va_arg(arg, void*);
	va_end(arg);

	gen_ioctl(fd, request, argp);
}

static int (*orig_open)(const char* filename, int flags, ...);
static int gen_open(const char* filename, int flags, mode_t mode);

int open(const char* filename, int flags, ...)
{
	mode_t mode;

	if (__OPEN_NEEDS_MODE(mode)) {
		va_list arg;
		va_start(arg, flags);
		mode = va_arg(arg, int);
		va_end(arg);
	}

	return gen_open(filename, flags, mode);
}

#include "wire_io_gen.c.inc"

static inline void set_nonblock(int fd)
{
        int ret = fcntl(fd, F_GETFL);
        if (ret < 0)
                return;

        fcntl(fd, F_SETFL, ret | O_NONBLOCK);
}

/* Return the performed action back to the caller */
static void return_action(struct wire_io* wire_io, struct wire_io_act *act)
{
	ssize_t ret = write(wire_io->response_send_fd, &act, sizeof(act));
	if (ret != sizeof(act))
		printf("wire_io: returning action failed in write, ret=%d errno=%d:  %m\n", (int)ret, errno);
}

/* Wait with an unlocked mutex on the condition until we are woken up, when we
 * are woken up the mutex is retaken and we can manipulate the list as we wish
 * and must ensure to unlock it and do it as fast as possible to reduce
 * contention.
 */
static struct wire_io_act *get_action(struct wire_io* wire_io)
{
	pthread_mutex_lock(&wire_io->mutex);

	while (list_empty(&wire_io->list)) {
		pthread_cond_wait(&wire_io->cond, &wire_io->mutex);
	}

	struct list_head *head = list_head(&wire_io->list);
	struct wire_io_act *entry = NULL;
	if (head) {
		list_del(head);
		entry = (struct wire_io_act*)list_entry(head, struct wire_io_act_common, elem);
	}

	pthread_mutex_unlock(&wire_io->mutex);

	return entry;
}

static void block_signals(void)
{
	sigset_t sig_set;

	sigfillset(&sig_set);
	pthread_sigmask(SIG_BLOCK, &sig_set, NULL);
}

/* The async thread implementation that waits for async actions to perform and runs them.
 */
static void *wire_io_thread(void *arg)
{
    struct wire_io* wire_io = (struct wire_io*)arg;
	block_signals();
	is_wire_thread = false;

	while (1) {
		struct wire_io_act *act = get_action(wire_io);
		if (!act) {
			continue;
		}

		perform_action(act);
		return_action(wire_io, act);
	}
	return NULL;
}

/* Take care of the response from the worker threads, gets the action that was
 * performed and resumes the caller to take care of the response.
 */
static void wire_io_response(void *UNUSED(arg))
{
	set_nonblock(wire_io.response_recv_fd);

	while (1) {
		static const unsigned MAX_RESPONSES = 32;
		struct wire_io_act *act[MAX_RESPONSES];
		bool go_to_sleep = false;
		ssize_t ret = orig_read(wire_io.response_recv_fd, act, sizeof(act));
		if (ret > 0) {
			unsigned i;
			const unsigned num_ret = ret / sizeof(act[0]);
			// Loop over all received responses
			for (i = 0; i < num_ret; i++) {
				// Wake each waiter
				wire_wait_resume(act[i]->common.wait);
				wire_io.num_active_ios--;
			}
			// We got less than the max number of responses so there is not
			// likely to be more to process imemdiately, go to sleep
			if (num_ret < MAX_RESPONSES)
				go_to_sleep = true;
		} else if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				go_to_sleep = true;
			} else {
				fprintf(stderr, "Error reading from socket for wire_io: %d = %m\n", errno);
				abort();
			}
		} else {
			fprintf(stderr, "EOF on the socketpair is highly improbable\n");
			abort();
		}

		if (go_to_sleep) {
			if (wire_io.num_active_ios == 0) {
				// No active io requests, do not hog the pending list
				// Also allows the process to exit cleanly when nothing else needs to happen
				wire_fd_mode_none(&wire_io.fd_state);
				wire_suspend();
			}

			// The fd state is set to read by the submitter in SEND_RET macro
			wire_wait_reset(&wire_io.fd_state.wait);
			wire_fd_wait(&wire_io.fd_state); // Wait for the response, only if we would block
		}
	}

	fprintf(stderr, "Closing down wire_io wire\n");
	wire_fd_mode_none(&wire_io.fd_state);
	close(wire_io.response_recv_fd);
	close(wire_io.response_send_fd);
	pthread_cond_destroy(&wire_io.cond);
	pthread_mutex_destroy(&wire_io.mutex);
}

static wire_t wire_io_first_run;
static char wire_io_first_run_stack[32];
static void wire_io_first_run_func(void* unused)
{
	// Upon initial run of this wire we will start using the wire_io for overridden io functions
	is_wire_thread = true;
}

void wire_io_init(int num_threads)
{
	wire_io.num_active_ios = 0;
	list_head_init(&wire_io.list);
	pthread_mutex_init(&wire_io.mutex, NULL);
	pthread_cond_init(&wire_io.cond, NULL);

	int sfd[2];
	int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sfd);
	if (ret < 0) {
		fprintf(stderr, "Error initializing a socketpair for wire_io: %m\n");
		abort();
	}

	wire_io.response_send_fd = sfd[0];
	wire_io.response_recv_fd = sfd[1];

	wire_fd_mode_init(&wire_io.fd_state, wire_io.response_recv_fd);
	wire_init(&wire_io.wire, "wire_io", wire_io_response, &wire_io, WIRE_STACK_ALLOC(4096));

	int i;
	for (i = 0; i < num_threads; i++) {
		pthread_t th;
		pthread_create(&th, NULL, wire_io_thread, &wire_io);
	}

	wire_init(&wire_io_first_run, "wire_io_first_run", wire_io_first_run_func, NULL, wire_io_first_run_stack, sizeof(wire_io_first_run_stack));
}

