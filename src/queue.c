#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "compat.h"
#include "io_uring.h"
#include "liburing.h"
#include "barrier.h"

static int __io_uring_get_completion(struct io_uring *ring,
				     struct io_uring_cqe **cqe_ptr, int wait)
{
	struct io_uring_cq *cq = &ring->cq;
	const unsigned mask = *cq->kring_mask;
	unsigned head;
	int ret;

	*cqe_ptr = NULL;
	head = *cq->khead;
	do {
		read_barrier();
		if (head != *cq->ktail) {
			*cqe_ptr = &cq->cqes[head & mask];
			break;
		}
		if (!wait)
			break;
		ret = io_uring_enter(ring->ring_fd, 0, 1,
					IORING_ENTER_GETEVENTS, NULL, _NSIG / 8);
		if (ret < 0)
			return -errno;
	} while (1);

	if (*cqe_ptr) {
		*cq->khead = head + 1;
		write_barrier();
	}

	return 0;
}

/*
 * Return an IO completion, if one is readily available
 */
int io_uring_get_completion(struct io_uring *ring,
			    struct io_uring_cqe **cqe_ptr)
{
	return __io_uring_get_completion(ring, cqe_ptr, 0);
}

/*
 * Return an IO completion, waiting for it if necessary
 */
int io_uring_wait_completion(struct io_uring *ring,
			     struct io_uring_cqe **cqe_ptr)
{
	return __io_uring_get_completion(ring, cqe_ptr, 1);
}

/*
 * Submit sqes acquired from io_uring_get_sqe() to the kernel.
 *
 * Returns number of sqes submitted
 */
int io_uring_submit(struct io_uring *ring)
{
	struct io_uring_sq *sq = &ring->sq;
	const unsigned mask = *sq->kring_mask;
	unsigned ktail, ktail_next, submitted;

	/*
	 * If we have pending IO in the kring, submit it first
	 */
	read_barrier();
	if (*sq->khead != *sq->ktail) {
		submitted = *sq->kring_entries;
		goto submit;
	}

	if (sq->sqe_head == sq->sqe_tail)
		return 0;

	/*
	 * Fill in sqes that we have queued up, adding them to the kernel ring
	 */
	submitted = 0;
	ktail = ktail_next = *sq->ktail;
	while (sq->sqe_head < sq->sqe_tail) {
		ktail_next++;
		read_barrier();
		if (ktail_next == *sq->khead)
			break;

		sq->array[ktail & mask] = sq->sqe_head & mask;
		ktail = ktail_next;

		sq->sqe_head++;
		submitted++;
	}

	if (!submitted)
		return 0;

	if (*sq->ktail != ktail) {
		write_barrier();
		*sq->ktail = ktail;
		write_barrier();
	}

submit:
	return io_uring_enter(ring->ring_fd, submitted, 0,
				IORING_ENTER_GETEVENTS, NULL, _NSIG / 8);
}

/*
 * Return an sqe to fill. Application must later call io_uring_submit()
 * when it's ready to tell the kernel about it. The caller may call this
 * function multiple times before calling io_uring_submit().
 *
 * Returns a vacant sqe, or NULL if we're full.
 */
struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring)
{
	struct io_uring_sq *sq = &ring->sq;
	unsigned next = sq->sqe_tail + 1;
	struct io_uring_sqe *sqe;

	/*
	 * All sqes are used
	 */
	if (next - sq->sqe_head > *sq->kring_entries)
		return NULL;

	sqe = &sq->sqes[sq->sqe_tail & *sq->kring_mask];
	sq->sqe_tail = next;
	return sqe;
}
