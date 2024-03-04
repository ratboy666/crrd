/* crrd.c
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
 * Fred Weigel, March 2024
 */

#define _XOPEN_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Internal times in the rrd are maintained as rrdt_t, which is in
 * seconds, milliseconds, microseconds... actual unit is dependent
 * on the system.
 */
typedef uint64_t rrdt_t;

typedef struct rrd {
    char *name;        /* name */
    rrdt_t resolution; /* time between successive entries */
    int capacity;      /* capacity of database */
    size_t size;       /* size of an entry */
    void *entries;     /* pointer to entries */
    int head;          /* head (beginning) */
    int tail;          /* tail (end) */
    rrdt_t start;      /* begin time of current bucket */
    rrdt_t last;       /* last update time */
    void (*zero)(struct rrd *);
    void (*update)(struct rrd *, void *);
} rrd_t;

/* Average
 */
static void default_update(rrd_t *r, void *pv) {
    fprintf(stderr, "update function not filled in\n");
    exit(EXIT_FAILURE);
}

/* Zero to tail
 */
static void default_zero(rrd_t *r) {
    fprintf(stderr, "zero function not filled in\n");
    exit(EXIT_FAILURE);
}

/* Convert timeval to rrdt_t. Since we may want to use blocks of time
 * less than a second, we convert timeval to rrdt_t. Type rrdt_t replaces
 * time_t
 */
rrdt_t tv2rrdt(struct timeval *tv) {
    return (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
}

/* Convert rrdt to timeval
 */
void rrdt2tv(struct timeval *tv, rrdt_t rt) {
    tv->tv_sec = rt / 1000;
    rt %= 1000;
    tv->tv_usec = rt * 1000;
}

/* If time were divided into equal-sized periods, tperiod, find_period()
 * returns the start of the period for the specific time.
 */ 
rrdt_t find_period(rrdt_t time, rrdt_t tperiod) {
    rrdt_t rem;
    rrdt_t start;

    rem = time % tperiod;
    time -= rem;
    return time;
}

/* Increment the tail (and head if necessary) by one position.
 */
void forward(rrd_t *r) {
    /* Bump tail, wrapping at capacity
     */
    ++r->tail;
    if (r->tail >= r->capacity)
	r->tail = 0;
    if (r->tail == r->head) {
        /* Tail hit head, bump head, wrapping at capacity
	 */
	++r->head;
	if (r->head >= r->capacity)
	    r->head = 0;
    }
    /* Update times.
     */
    r->start = find_period(r->start + r->resolution + 1, r->resolution); 
}

/* Create a new rrd of capacity with resolution res
 */
rrd_t *rrd_create(char *s, struct timeval *res, unsigned cap, size_t sz) {
    rrd_t *r;

    r = malloc(sizeof (struct rrd));
    if (r == NULL) {
	return NULL;
    }
    r->name = s;
    r->resolution = tv2rrdt(res);
    /* Allocate the database -- get some temporaries too
     */
    r->entries = malloc(cap * sz);
    if (r->entries == NULL) {
	return NULL;
    }
    r->capacity = cap;
    r->size = sz;
    r->head = r->tail = -1;
    r->update = default_update;
    r->zero = default_zero;
    return r;
}

/* Return length of data in the rrd. rrd_get works from 0..rrd_len()-1
 */
unsigned rrd_len(rrd_t *r) {
    if (r->tail < 0)
	return 0;
    if (r->head <= r->tail)
	return r->tail - r->head + 1;
    if (r->head > r->tail)
	return r->capacity - r->head + r->tail + 1;
    exit(EXIT_FAILURE);
    return 0;
}

/* Return resolution - type rrdt_t Usually in milliseconds, but may
 * be another unit, depending on application.
 */
rrdt_t rrd_resolution(rrd_t *r) {
    return r->resolution;
}

/* Return capacity
 */
int rrd_capacity(rrd_t *r) {
    return r->capacity;
}

/* Debug information on rrd
 */
void rrd_debug(rrd_t *r) {
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
}

/* Destroy the rrd database
 */
void rrd_destroy(rrd_t *r) {
    free(r->entries);
    free(r);
}

/* Store value into rrd at tail
 */
void rrd_store(rrd_t *r, void *v) {
    memcpy(r->entries + (r->tail * r->size), v, r->size);
    float val = *(float *)v;
}

/* Add value to rrd at specified time. Data will be consolidated
 * to apply data with any timestamp into the defined periods of
 * the rrd
 */
void rrd_add_at(rrd_t *r, void *v, struct timeval *tv) {
    rrdt_t t, t0;

    /* Convert struct timeval to rrdt_t
     */
    t = tv2rrdt(tv);

    /* t0, t1 is the period for this time
     */
    t0 = find_period(t, r->resolution);

    /* Empty rrd, put in first element
     */
    if (r->tail < 0) {

	r->head = r->tail = 0;
	rrd_store(r, v);
	r->start = t0;
	r->last = t;
	return;
    }

    /* Cannot go back in time
     */
    if (t < r->last) {
	return;
    }

    /* Are we in current period? Yes, do running average.
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

    /* One or more periods in the future. Skip forward,
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

/* Return value at indicated index -- returns a void * to the data
 */
void *rrd_get(rrd_t *r, int i) {
    int n;

    if ((i < 0) || (i >= rrd_len(r))) {
	return NULL;
    }

    n = r->head + i;
    if (n >= r->capacity)
	n -= r->capacity;
    return r->entries + (n * r->size);
}

/* Add value at the current time
 */
void rrd_add(rrd_t *r, void *v) {
    struct timeval now;

    gettimeofday(&now, NULL);
    rrd_add_at(r, v, &now);
}

void rrd_setfunctions(rrd_t *r, void *fupdate, void *fzero) {
    r->update = fupdate;
    r->zero = fzero;
}

/* ================================================================= */

void period_tests(void) {
    struct tm tm;
    char *s;
    char buf[256];
    time_t t;
    struct timeval tv;
    rrdt_t in, tperiod, good_start;
    rrdt_t start;
    int fails = 0;

    struct {
	char *in;
	int tperiod;
	char *start;
	char *stop;
    } tests[] = {
	{ "2024-01-02T10:04:10Z", 30, "2024-01-02T10:04:00Z" },
	{ "2024-01-02T10:04:29Z", 30, "2024-01-02T10:04:00Z" },
	{ "2024-01-02T10:04:30Z", 30, "2024-01-02T10:04:30Z" },
	{ "2024-01-02T10:04:10Z", 60, "2024-01-02T10:04:00Z" },
	{ "2024-01-02T10:04:10Z", 3600 /* hour */, "2024-01-02T10:00:00Z" },
	{ "2024-01-02T10:04:10Z", 86400 /* day */, "2024-01-02T00:00:00Z" },
        { NULL, 0, NULL }
    };

    /* Very odd effect in gcc library -- if I do not do this, the
     * actual test data fails ?!? It appears that tm is not fully
     * populated by (eg) strptime(). This may be a bug in GNU
     * strptime(). Note that we do set TZ=GMT as well.
     *
     * Seeding tm with gmtime appears to "do the trick".
     */
    putenv("TZ=GMT");
    t = 0;
    tm = *gmtime(&t);

    fprintf(stderr, "period tests\n");
    putenv("TZ=GMT");
    for (int i = 0; tests[i].in != NULL; ++i) {
	strptime(tests[i].in, "%Y-%m-%dT%H:%M:%SZ", &tm);
	t = mktime(&tm);
	tv.tv_sec = t;
	tv.tv_usec = 0;
	in = tv2rrdt(&tv);

	strptime(tests[i].start, "%Y-%m-%dT%H:%M:%SZ", &tm);
	t = mktime(&tm);
	tv.tv_sec = t;
	tv.tv_usec = 0;
	good_start = tv2rrdt(&tv);

        tv.tv_sec = tests[i].tperiod;
	tv.tv_usec = 0;
	tperiod = tv2rrdt(&tv);

	start = find_period(in, tperiod);

	if (start == good_start)
	    fprintf(stderr, "  test %d OK\n", i);
	else {
	    fprintf(stderr, "  test %d FAIL\n", i);
	    ++fails;
	}

	fprintf(stderr, "  good in    %s %i\n", tests[i].in, tests[i].tperiod);
	rrdt2tv(&tv, start);
	t = tv.tv_sec;
	strftime(buf, 256, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
	fprintf(stderr, "  in         %s %lu\n", buf, start);

	fprintf(stderr, "  good start %s %lu\n", tests[i].start, good_start);
	rrdt2tv(&tv, start);
	t = tv.tv_sec;
	strftime(buf, 256, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
	fprintf(stderr, "  start      %s %lu\n", buf, start);
    }
    if (fails != 0) {
	fprintf(stderr, "failure(s) in period_tests\n");
	exit(EXIT_FAILURE);
    }
    fprintf(stderr, "period_tests complete\n");
}

static void update(rrd_t r, void *v) {
}

static void zero(rrd_t r) {
}

void simple_test(void) {
    rrd_t *r;
    struct timeval resolution = { 1, 0 };
    double v;
    double *p;

    fprintf(stderr, "simple_test\n");
    r = rrd_create("test", &resolution, 10, sizeof (double));
    if (r == NULL) {
	fprintf(stderr, "rrd_create failed\n");
	exit(EXIT_FAILURE);
    }
    rrd_setfunctions(r, update, zero);

    rrd_debug(r);

    if (rrd_len(r) != 0) {
        fprintf(stderr, "new rrd is not empty\n");
	exit(EXIT_FAILURE);
    }

    v = 0.0;
    rrd_add(r, &v);
    if (rrd_len(r) != 1) {
        fprintf(stderr, "rrd not length 1\n");
	exit(EXIT_FAILURE);
    }

    p = rrd_get(r, 0);
    v = *p;
    if (v != 0.0) {
        fprintf(stderr, "rrd got %g wanted %g\n", v, 0.0);
	exit(EXIT_FAILURE);
    }

    rrd_destroy(r);

    fprintf(stderr, "simple_test complete\n");
}

/* avg -= avg / N;
 * avg += new_sample / N;
 *
 * If changed by M values
 * new average =
 *  old average * (n-len(M))/n + (sum of values in M)/n
 *
 *  https://stackoverflow.com/questions/12636613/how-to-calculate-moving-average-without-keeping-the-count-and-data-total
 */

/* update is called to update the rolling average in a period. This
 * implements a simple rolling average.
 */
void f_update(rrd_t *r, void *pv) {
    float v, old, new;
    rrdt_t res;

    v = *(float *)pv;
    old = *(float *)(r->entries + (r->tail * r->size)); 
    res = r->resolution / 1000;
    new = old;
    new -= new / res;
    new += v / res;

    rrd_store(r, &new);
}

static void f_zero(rrd_t *r) {
    float z = 0.0;
    rrd_store(r, &z);
}

void complex_test(void) {
    rrd_t *r;
    struct timeval resolution = { 30, 0 };
    time_t t;
    struct timeval tv;
    struct tm tm;
    float v;
    float *p;
    int fails = 0;

    struct {
        char *ts;
	float val;
    } input[] = {
        /* 13 entries, should sort into 10 periods */
	/*                                     i     start  stop  */
	{ "2024-01-01T08:10:01Z", 5.0   }, /*  0 1  10:00 - 10:30 */
        { "2024-01-01T08:10:30Z", 5.0   }, /*  1 2  10:30 - 11:00 */
        { "2024-01-01T08:10:45Z", 5.0   }, /*  2 2  10:30 - 11:00 */
        { "2024-01-01T08:11:00Z", 5.0   }, /*  3 3  11:00 - 11:30 */
        { "2024-01-01T08:11:15Z", 10.0  }, /*  4 3  11:00 - 11:30 */

        { "2024-01-01T08:11:35Z", 15.0  }, /*  5 4  11:30 - 12:00 */
        { "2024-01-01T08:11:40Z", 8.0   }, /*  6 4  11:30 - 12:00 */
        { "2024-01-01T08:11:42Z", 305.0 }, /*  7 4  11:30 - 12:00 */
        { "2024-01-01T08:12:04Z", 10.0  }, /*  8 5  12:00 - 12:30 */
        { "2024-01-01T08:13:34Z", 20.0  }, /*  9 6  12:30 - 13:00 */
	                                   /*    7  13:00 - 13:30 */
        { "2024-01-01T08:14:05Z", 30.0  }, /* 10 8  14:00 - 14:30 */
        { "2024-01-01T08:14:35Z", 30.0  }, /* 11 9  14:30 - 15:00 */
        { "2024-01-01T08:15:20Z", 20.0  }, /* 12 10 15:00 - 15:30 */
        { NULL, 0.0 },
    };
    struct {
        int index;
	float val;
    } expected[] = {
	{ 0, 5.0          },
	{ 1, 5.166666985  },
	{ 2, 24.44111252  },
	{ 3, 10.0         },
	{ 4, 0.0          },
	{ 5, 0.0          },
	{ 6, 20.0         },
	{ 7, 30.0         },
	{ 8, 30.0         },
	{ 9, 20.0         },
	{ -1, 0.0 }
    };

    fprintf(stderr, "complex_test\n");
    r = rrd_create("test", &resolution, 10, sizeof (float));
    if (r == NULL) {
	fprintf(stderr, "rrd_create failed\n");
	exit(EXIT_FAILURE);
    }
    rrd_setfunctions(r, f_update, f_zero);

    putenv("TZ=GMT");
    t = 0;
    tm = *gmtime(&t);

    fprintf(stderr, "adding input data\n");
    for (int i = 0; input[i].ts != NULL; ++i) {
	strptime(input[i].ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
	t = mktime(&tm);
	tv.tv_sec = t;
	tv.tv_usec = 0;
	fprintf(stderr, "%2d %s %lu %g\n", i, input[i].ts, t, input[i].val);
	rrd_add_at(r, (void *)&(input[i].val), &tv);
	fprintf(stderr, "  len = %d\n", rrd_len(r));
    }

    if (rrd_len(r) != 10) {
	fprintf(stderr, "error len = %d\n", rrd_len(r));
	exit(EXIT_FAILURE);
    }

    fprintf(stderr, "getting data\n");
    for (int i; expected[i].index >= 0; ++i) {
        p = rrd_get(r, expected[i].index);
    	if (p == NULL)
    	    fprintf(stderr, "rrd_get returned NULL\n");
	v = 0.0;
    	v = *p;
	fprintf(stderr, "complex %i %20.10g %20.10g\n", expected[i].index,
	                                          v,
	                                          expected[i].val);
	if (v != expected[i].val) {
	    fprintf(stderr, "--- %d\n", expected[i].index);
	    ++fails;
	}
    }

    rrd_destroy(r);

    if (fails != 0) {
	fprintf(stderr, "complex_test failed\n");
	exit(EXIT_FAILURE);
    }
    fprintf(stderr, "complex_test complete\n");
}

int main(int ac, char **av) {
    printf("crrd - C RRD Database\n");
    period_tests();
    simple_test();
    complex_test();
    return EXIT_SUCCESS;
}

