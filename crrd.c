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
 * TESTING defined means we are running in user space for testing.
 *
 * Fred Weigel, March 2024
 * From an idea by Allan Jude
 */

#ifdef TESTING
#  include <stddef.h>
#  include <string.h>
#  include <stdlib.h>
#  include <stdio.h>
#  include <time.h>
#  include "crrd.h"
#else
#  include <sys/zfs_context.h>
#  include <sys/crrd.h>
#endif

/* Average */
static void
default_update(rrd_t *r, void *pv)
{
	r = r; pv = pv;
#ifdef TESTING
	fprintf(stderr, "update function not filled in\n");
	exit(EXIT_FAILURE);
#endif
}

/* Zero to tail */
static
void default_zero(rrd_t *r, void *pv)
{
	r = r; pv = pv;
#ifdef TESTING
	fprintf(stderr, "zero function not filled in\n");
	exit(EXIT_FAILURE);
#endif
}

/*
 * If time were divided into equal-sized periods of duration tperiod,
 * find_period() returns the start of the period for the specific time.
 * The next period is period + tperiod.
 */ 
static hrtime_t
find_period(hrtime_t time, hrtime_t tperiod)
{
	hrtime_t rem;

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

/* Return tail of rrd */
int
rrd_tail(rrd_t *r)
{
	return (r->tail);
}

/* Create a new rrd of capacity with resolution res */
rrd_t *
rrd_create(char *s, hrtime_t res, unsigned cap, size_t sz)
{
	rrd_t *r;
	size_t asize;

	asize = sizeof (struct rrd) + (cap * sz);
#ifdef TESTING
	r = malloc(sizeof (struct rrd) + (cap * sz));
#else
	r = kmem_alloc(asize, KM_SLEEP);	
#endif
	if (r == NULL) {
		return (NULL);
	}
	r->name = s;
	r->asize = asize;
	r->resolution = res;
	r->next = NULL;
	r->start = r->last = 0;
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
#ifdef TESTING
	fprintf(stderr, "rrd_len: impossible\n");
	exit(EXIT_FAILURE);
#endif
	return (0);
}

/* * Return resolution */
hrtime_t
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
#ifdef TESTING
	fprintf(stderr, "rrd_debug:    %p\n",  r);
	fprintf(stderr, "  name:       %s\n",  r->name);
	fprintf(stderr, "  resolution: %ld\n", r->resolution);
	fprintf(stderr, "  head:       %d\n",  r->head);
	fprintf(stderr, "  tail:       %d\n",  r->tail);
	fprintf(stderr, "  start:      %ld\n", r->start);
	fprintf(stderr, "  last:       %ld\n", r->last);
	fprintf(stderr, "  entries:    %p\n",  r->entries);
	fprintf(stderr, "  size:       %lu\n", r->size);
	fprintf(stderr, "  len:        %d\n",  rrd_len(r));
#else
	r = r;
#endif
}

/* Destroy the rrd database */
void
rrd_destroy(rrd_t *r)
{
	if (r) {
#ifdef TESTING
		free(r);
#else
		kmem_free(r, r->asize);
#endif
	}
}

/* Store value into rrd at tail */
static
void rrd_store(rrd_t *r, void *v)
{
	memcpy((char *)r->entries + (r->tail * r->size), v, r->size);
}

/*
 * Add value to rrd at specified time. Data will be consolidated
 * to apply data with any timestamp into the defined periods of
 * the rrd
 */
void
rrd_add_at(rrd_t *r, void *v, hrtime_t t)
{
	hrtime_t t0;

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
	 * All calculation for the "running average" or other
	 * accumulation is pushed into update().
	 *
	 * We may sill want to smear the past which is not
	 * accomodated yet.
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
		/*
		 * "Zero" is a bit of a misnomer. For txg, we want to plant
		 * the previous txg. For calculation, we want to plant either
		 * the present or previous value.
		 */
		(r->zero)(r, v);
	}
	rrd_store(r, v);
	r->start = t0;
	r->last = t;
}

/* Return entry pointer for index n */
void *
rrd_entry(rrd_t *r, int i)
{
	return (char *)r->entries + (i * r->size);
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
	return rrd_entry(r, n);
}

/* Add value at the current time */
void
rrd_add(rrd_t *r, void *v)
{
	hrtime_t t;

#ifdef TESTING
#  define SEC2NSEC(n) (n * 1000LL * 1000LL * 1000LL)
#  define USEC2NSEC(n) (n * 1000LL)
	struct timeval now;
	gettimeofday(&now, NULL);
	t = SEC2NSEC(now.tv_sec) + USEC2NSEC(now.tv_usec);
#else
	t = gethrtime();
#endif
	rrd_add_at(r, v, t);
}

/* Set callbacks */
void
rrd_setfunctions(rrd_t *r, void *fupdate, void *fzero)
{
	r->update = fupdate;
	r->zero = fzero;
}

/*
 * The rrd_find function looks in the rrd for the time t. It returns
 * the value from the tightest period that contains the specified
 * time. Value and resolution are returned. Return is 1 if we have
 * data, 0 if not.
 *
 * The rrds are linked together -- most precise first, and ordered.
 * We find the first rrd that covers our time.
 */
int dbrrd_query(rrd_t *r, hrtime_t tv, void **vp, hrtime_t *res)
{
	hrtime_t t0, start;
	int i;

	/* Find for time in future fails */
	if (tv > r->last) {
		return (0);
	}

	/*
	 * If the rrd is empty, there is no data. Since all rrds are
	 * added to "in parallel", they are all empty (or not)
	 */
	if (rrd_len(r) == 0) {
		return (0);
	}

	while (r != NULL) {

		t0 = find_period(tv, r->resolution);

		/*
		 * Time start for this rdd (may not be full). r->start
		 * is the start of the active period.
		 */
		start = r->start - (r->resolution * (rrd_len(r) - 1));

		/*
		 * Is the query time within the coverage of this rrd?
		 * Since the rrds are to be linked in increasing period
		 * the first match will be the most precise one.
		 */
		if (t0 >= start) {
			i = (t0 - start) / r->resolution;
			*vp = rrd_get(r, i);
			*res = r->resolution;
			return (1);
		}

		/* Query time is out of this rrd, try next rrd (which
		 * is a bit "fuzzier".
		 */
		r = r->next;
	}

	/* Too old, no record */
	return (0);
}

void
dbrrd_add_at(rrd_t *r, void *vp, hrtime_t t)
{
	while (r != NULL) {
	    rrd_add_at(r, vp, t);
	    r = r->next;
	}
}

void
dbrrd_add(rrd_t *r, void *v)
{
	hrtime_t t;

#ifdef TESTING
	struct timeval now;
	gettimeofday(&now, NULL);
	t = SEC2NSEC(now.tv_sec) + USEC2NSEC(now.tv_usec);
#else
	t = gethrtime();
#endif

	dbrrd_add_at(r, v, t);
}

void
dbrrd_destroy(rrd_t *h)
{
	rrd_t *p;

	while (h != NULL) {
	    p = h->next;
	    rrd_destroy(h);
	    h = p;
	}
}

rrd_t *
dbrrd_create(char *name, dbrrd_spec_t *p, size_t sz, void *update, void *zero)
{
	rrd_t *h;
	rrd_t *r;

	h = NULL;
	while (p->capacity > 0) {
		r = rrd_create(name, p->tv, p->capacity, sz);
		if (r == NULL) {
#ifdef TESTING
			fprintf(stderr, "rrd_create failed\n");
#endif
			if (h != NULL) {
				dbrrd_destroy(h);
			}
			return NULL;
		}
		rrd_setfunctions(r, update, zero);
		r->next = h;
		h = r;
		++p;
	}
	return h;
}
