/*
 * crrd.c
 *
 * Implemention of a round-robin time-series (RRD) Database
 *
 * Yes, it's pronounced exactly like that!
 *
 * Strange shennanigans with value being opaque is because this code
 * is meant to go into ZFS. ZFS is not GPL, and so cannot use floats.
 * As a result, the data type and the averaging calculation are not
 * "hard coded" here.
 *
 * Very simple moving average is implemented in the test case.
 * For use with Time to TXG conversion, no average at all is needed
 * (just do not overwrite the first recorded txg in the time period).
 *
 * To use this, create multiple rrds -- one for each time unit.
 * For example a seconds rrd with 60 slots, and a resolution of one
 * second. A minutes rrd with 60 slots and a resolution of 60 seconds.
 * Repeat with 60 slots for hour, 24 hours per day, 365 days per year,
 * and, say 20 years.
 *
 * Data consumption will be on the order of 100 bytes per rrd. 6 rrds
 * (as in the previous paragraph) would then be ~600 bytes. Each requires
 * (size) bytes per entry -- say 8 or 10 bytes. Under 10K per year!
 *
 * Each data point needs to be added to each of the rrds. Each rrd
 * simply uses a wider period. The slots are used round robin.
 *
 * Two compile modes: STANDALONE and KERNEL. In STANDALONE, runs in
 * user space. KERNEL runs in the kernel.
 *
 * Uses malloc() free() memcpy() are used in both modes.
 *
 * Fred Weigel, March 2024
 */

/*#define STANDALONE*/

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#ifdef STANDALONE
#include <stdio.h>
#include <time.h>
#endif
#include "crrd.h"

/* Average */
static void
default_update(rrd_t *r, void *pv)
{
#ifdef STANDALONE
	fprintf(stderr, "update function not filled in\n");
	exit(EXIT_FAILURE);
#endif
}

/* Zero to tail */
static
void default_zero(rrd_t *r)
{
#ifdef STANDALONE
	fprintf(stderr, "zero function not filled in\n");
	exit(EXIT_FAILURE);
#endif
}

/*
 * Convert timeval to rrdt_t. Since we may want to use blocks of time
 * less than a second, we convert timeval to rrdt_t. Type rrdt_t replaces
 * time_t
 */
rrdt_t
tv2rrdt(struct timeval *tv)
{
	return (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
}

/* Convert rrdt to timeval */
void
rrdt2tv(struct timeval *tv, rrdt_t rt)
{
	tv->tv_sec = rt / 1000;
	rt %= 1000;
	tv->tv_usec = rt * 1000;
}

/*
 * If time were divided into equal-sized periods of duration tperiod,
 * find_period() returns the start of the period for the specific time.
 * The next period is period + tperiod.
 */ 
static rrdt_t
find_period(rrdt_t time, rrdt_t tperiod)
{
	rrdt_t rem;
	rrdt_t start;

	rem = time % tperiod;
	time -= rem;
	return (time);
}

/* Increment the tail (and head if necessary) by one position. */
static
void forward(rrd_t *r)
{
	/* Bump tail, wrapping at capacity */
	++r->tail;
	if (r->tail >= r->capacity) {
		r->tail = 0;
	}
	if (r->tail == r->head) {
		/* Tail hit head, bump head, wrapping at capacity */
		++r->head;
		if (r->head >= r->capacity) {
			r->head = 0;
		}
	}
	/* Update times */
	r->start = find_period(r->start + r->resolution + 1, r->resolution); 
}

/* Create a new rrd of capacity with resolution res */
rrd_t *
rrd_create(char *s, struct timeval *res, unsigned cap, size_t sz)
{
	rrd_t *r;

	r = malloc(sizeof (struct rrd));
	if (r == NULL) {
		return (NULL);
	}
	r->name = s;
	r->resolution = tv2rrdt(res);
	r->next = NULL;
	r->start = r->last = 0;
	/* Allocate the database
	 */
	r->entries = malloc(cap * sz);
	if (r->entries == NULL) {
	    free(r);
	    return (NULL);
	}
	r->capacity = cap;
	r->size = sz;
	r->head = r->tail = -1;
	r->update = default_update;
	r->zero = default_zero;
	return (r);
}

/* Return length of data in the rrd. rrd_get works from 0..rrd_len()-1 */
unsigned
rrd_len(rrd_t *r)
{
	if (r->tail < 0) {
		return (0);
	}
	if (r->head <= r->tail) {
		return (r->tail - r->head + 1);
	}
	if (r->head > r->tail) {
		return (r->capacity - r->head + r->tail + 1);
	}
#ifdef STANDALONE
	fprintf(stderr, "rrd_len: impossible\n");
	exit(EXIT_FAILURE);
#endif
	return (0);
}

/*
 * Return resolution - type rrdt_t Usually in milliseconds, but may
 * be another unit, depending on application.
 */
rrdt_t
rrd_resolution(rrd_t *r)
{
	return (r->resolution);
}

/* Return capacity */
int
rrd_capacity(rrd_t *r)
{
	return (r->capacity);
}

/* Debug information on rrd */
void
rrd_debug(rrd_t *r)
{
#ifdef STANDALONE
	fprintf(stderr, "rrd_debug:    %p\n",  r);
	fprintf(stderr, "  name:       %s\n",  r->name);
	fprintf(stderr, "  resolution: %ld\n", r->resolution);
	fprintf(stderr, "  head:       %d\n",  r->head);
	fprintf(stderr, "  tail:       %d\n",  r->tail);
	fprintf(stderr, "  start:      %ld\n", r->start);
	fprintf(stderr, "  last:       %ld\n", r->last);
	fprintf(stderr, "  entries:    %lp\n", r->entries);
	fprintf(stderr, "  size:       %u\n",  r->size);
	fprintf(stderr, "  len:        %d\n",  rrd_len(r));
#endif
}

/* Destroy the rrd database */
void
rrd_destroy(rrd_t *r)
{
	free(r->entries);
	free(r);
}

/* Store value into rrd at tail */
static
void rrd_store(rrd_t *r, void *v)
{
	memcpy(r->entries + (r->tail * r->size), v, r->size);
}

/*
 * Add value to rrd at specified time. Data will be consolidated
 * to apply data with any timestamp into the defined periods of
 * the rrd
 */
void
rrd_add_at(rrd_t *r, void *v, struct timeval *tv)
{
	rrdt_t t, t0;

	/* Convert struct timeval to rrdt_t */
	t = tv2rrdt(tv);

	/*
	 * t0 is the beginning of the period for this time
	 * t0 + resolution is one past the end
	 */
	t0 = find_period(t, r->resolution);

	/* Empty rrd, put in first element */
	if (r->tail < 0) {
		r->head = r->tail = 0;
		rrd_store(r, v);
		r->start = t0;
		r->last = t;
		return;
	}

	/* Cannot go back in time */
	if (t < r->last) {
		return;
	}

	/*
	 * Are we in current period? Yes, do running average.
	 *
	 * FIXME - note that we keep running average (or whatever
	 * it is programmed) ONLY for a single period. The sample
	 * may extend into the next period, or last! But, we cannot
	 * really know .. we could smear a bit into the past, or
	 * open the next period... but, I want to push all of that
	 * (if needed) into the update() function used.
	 */
	if (t0 == r->start) {
		r->start = t0;
		r->last = t;
		(r->update)(r, v);
		return;
	}

	/*
	 * One or more periods in the future. Skip forward,
	 * then store.
	 */
	while (r->start < t0) {
		forward(r);
		(r->zero)(r);
	}
	rrd_store(r, v);
	r->start = t0;
	r->last = t;
}

/* Return value at indicated index -- returns a void * to the data */
void *
rrd_get(rrd_t *r, int i)
{
	int n;

	if ((i < 0) || (i >= rrd_len(r))) {
		return NULL;
	}

	n = r->head + i;
	if (n >= r->capacity) {
		n -= r->capacity;
	}
	return r->entries + (n * r->size);
}

/* Add value at the current time */
void
rrd_add(rrd_t *r, void *v)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	rrd_add_at(r, v, &now);
}

/* Set callbacks */
void
rrd_setfunctions(rrd_t *r, void *fupdate, void *fzero)
{
	r->update = fupdate;
	r->zero = fzero;
}

