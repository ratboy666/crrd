/*
 * test.c
 *
 * Test functions for crrd
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
 * From an idea by Allan Jude
 */

#define _XOPEN_SOURCE
#define STANDALONE

#include "crrd.c"

void
period_test(void)
{
	struct tm tm;
	char buf[256];
	time_t t;
	struct timeval tv;
	rrdt_t start;
	rrdt_t in, tperiod, good_start;
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

	/*
	 * Very odd effect in gcc library -- if I do not do this, the
	 * actual test data fails ?!? It appears that tm is not fully
	 * populated by (eg) strptime(). This may be a bug in GNU
	 * strptime(). Note that we do set TZ=GMT as well.
	 *
	 * Seeding tm with gmtime appears to "do the trick".
	 */
	putenv("TZ=GMT");
	t = 0;
	tm = *gmtime(&t);

	fprintf(stderr, "period_test\n");
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

		if (start == good_start) {
			fprintf(stderr, "  test %d OK\n", i);
		} else {
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

static void
update(rrd_t r, void *v)
{
}

static void
zero(rrd_t r, void *v)
{
}

void
simple_test(void)
{
	rrd_t *r;
	struct timeval resolution = { 1, 0 };
	double v;
	double *p;

	fprintf(stderr, "simple_test\n");
	r = rrd_create("simple", &resolution, 10, sizeof (double));
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

/*
 * avg -= avg / N;
 * avg += new_sample / N;
 *
 *  https://stackoverflow.com/questions/12636613/how-to-calculate-moving-average-without-keeping-the-count-and-data-total
 */

/* update is called to update the rolling average in a period. This
 * implements a simple rolling average.
 */
void
f_update(rrd_t *r, void *pv)
{
	float v, old, new;
	rrdt_t res;

	v = *(float *)pv;
	old = *(float *)rrd_entry(r, rrd_tail(r));
	res = r->resolution / 1000;
	new = old;
	new -= new / res;
	new += v / res;

	rrd_store(r, &new);
}

static void
f_zero(rrd_t *r, void *p)
{
    float v = *(float *)p;
    rrd_store(r, &v);
}

void
complex_test(void)
{
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
		{ 4, 20.0         },
		{ 5, 20.0         },
		{ 6, 20.0         },
		{ 7, 30.0         },
		{ 8, 30.0         },
		{ 9, 20.0         },
		{ -1, 0.0 }
	};

	fprintf(stderr, "complex_test\n");
	r = rrd_create("complex", &resolution, 10, sizeof (float));
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
	for (int i = 0; expected[i].index >= 0; ++i) {
		p = rrd_get(r, expected[i].index);
		if (p == NULL) {
			fprintf(stderr, "rrd_get returned NULL\n");
			++fails;
			continue;
		}
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

void
dbrrd_test(void)
{
	rrd_t *h;
	struct timeval tv = { 0, 0 };
	struct timeval res;
	float v;
	int n;
	void *p;
	/* Must be sorted descending by timeval */
	dbrrd_spec_t dbrrd_periods[] = {
		{ 100, { 1000, 0 } }, 
		{ 100, {  100, 0 } }, 
		{ 100, {   10, 0 } }, 
		{ 100, {    1, 0 } }, 
		{ 0, { 0, 0 } }, 
	};

#define LIMIT 150000

	fprintf(stderr, "dbrrd_test\n");
	h = dbrrd_create("dbrrd", dbrrd_periods, sizeof(float),
		f_update, f_zero);

	/*
	 * For 0..LIMIT-1 seconds, add 5.0. All averages should be 5.0,
	 * and we will try retreival going back in time for each of
	 * 1, 10, 100, and 1000 second rrds
	 */
	for (int i = 0; i < LIMIT; ++i) {
		v = 5.0;
		tv.tv_sec = i;
		dbrrd_add_at(h, &v, &tv);
	}

	/* This query is in the future */
	tv.tv_sec = LIMIT + 1;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		fprintf(stderr, "future query allowed\n");
		exit(EXIT_FAILURE);
	}

	/* We query limit of each rrd */

	/* multi1 covers 1..100 seconds */
	tv.tv_sec = LIMIT - 1;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		v = *(float *)p;
		fprintf(stderr, "%10ld %g +-%ld seconds\n",
			tv.tv_sec, v, res.tv_sec / 2);
	} else {
		fprintf(stderr, "fail sec = %ld\n", tv.tv_sec);
		exit(EXIT_FAILURE);
	}
	tv.tv_sec = LIMIT - 100;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		v = *(float *)p;
		fprintf(stderr, "%10ld %g +-%ld seconds\n",
			tv.tv_sec, v, res.tv_sec / 2);
	} else {
		fprintf(stderr, "fail sec = %ld\n", tv.tv_sec);
		exit(EXIT_FAILURE);
	}

	/* multi10 covers 1..1000 seconds */
	tv.tv_sec = LIMIT - 100 - 1;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		v = *(float *)p;
		fprintf(stderr, "%10ld %g +-%ld seconds\n",
			tv.tv_sec, v, res.tv_sec / 2);
	} else {
		fprintf(stderr, "fail sec = %ld\n", tv.tv_sec);
		exit(EXIT_FAILURE);
	}
	tv.tv_sec = LIMIT - 1000;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		v = *(float *)p;
		fprintf(stderr, "%10ld %g +-%ld seconds\n",
			tv.tv_sec, v, res.tv_sec / 2);
	} else {
		fprintf(stderr, "fail sec = %ld\n", tv.tv_sec);
		exit(EXIT_FAILURE);
	}

	/* multi100 covers 1..10000 seconds */
	tv.tv_sec = LIMIT - 1000 - 1;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		v = *(float *)p;
		fprintf(stderr, "%10ld %g +-%ld seconds\n",
			tv.tv_sec, v, res.tv_sec / 2);
	} else {
		fprintf(stderr, "fail sec = %ld\n", tv.tv_sec);
		exit(EXIT_FAILURE);
	}
	tv.tv_sec = LIMIT - 10000;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		v = *(float *)p;
		fprintf(stderr, "%10ld %g +-%ld seconds\n",
			tv.tv_sec, v, res.tv_sec / 2);
	} else {
		fprintf(stderr, "fail sec = %ld\n", tv.tv_sec);
		exit(EXIT_FAILURE);
	}

	/* multi1000 covers 1..100000 seconds */
	tv.tv_sec = LIMIT - 10000 - 1;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		v = *(float *)p;
		fprintf(stderr, "%10ld %g +-%ld seconds\n",
			tv.tv_sec, v, res.tv_sec / 2);
	} else {
		fprintf(stderr, "fail sec = %ld\n", tv.tv_sec);
		exit(EXIT_FAILURE);
	}
	tv.tv_sec = LIMIT - 100000;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		v = *(float *)p;
		fprintf(stderr, "%10ld %g +-%ld seconds\n",
			tv.tv_sec, v, res.tv_sec / 2);
	} else {
		fprintf(stderr, "fail sec = %ld\n", tv.tv_sec);
		exit(EXIT_FAILURE);
	}

	/* This query is earlier than our time space */
	tv.tv_sec = 150000 - 100000 - 1;
	n = dbrrd_query(h, &tv, &p, &res);
	if (n) {
		fprintf(stderr, "should have failed at sec = %ld\n", tv.tv_sec);
		exit(EXIT_FAILURE);
	}

	dbrrd_destroy(h);
	fprintf(stderr, "dbrrd_test complete\n");
}

typedef struct txg_store {
	uint64_t l;
	uint64_t h;
} txg_store_t;

/*
 * Update is called to update the rolling average in a period. This
 * implements a "broadening" of txg range, as averaging makes no
 * sense.
 */
void
txg_update(rrd_t *r, void *pv)
{
	/*
	 * Averaging txgs make no sense at all.
	 *
	 * In general, we want to return the earliest or latest txg in
	 * a given time period. The txg is to be used to bracket a ZFS
	 * scrub operation (from:to). The from should be the earliest.
	 * The to should be the latest. This is arguable! We could
	 * save TWO txgs per time bracket. And... crrd could return
	 * them both. Value is, from the perspective of crrd, just a
	 * void* and that gives flexibility (could be a single value,
	 * or even a struct).
	 *
	 * Now, txg_update() makes sense because it holds the discrete
	 * (low/high) txg for each time bracket. Note that the merging
	 * is very simple: we just keep the smallest and largest!
	 * Now, on entry for a txg, there sill be only one txg value
	 * entered AT THIS TIME. simply set both low and high to the
	 * same. The "smearing" occurs as we enter the value into the
	 * wider periods.
	 */
	txg_store_t new;
	txg_store_t old;

	old = *(txg_store_t *)rrd_entry(r, rrd_tail(r));
	new = *(txg_store_t *)pv;

	/* Broaden new to cover old */
	if (old.l < new.l)
		new.l = old.l;
	if (old.h > new.h)
		new.h = old.h;
	rrd_store(r, &new);
}

/*
 * This is used to fill time periods. We always have a "previous" time
 * slot, so it is safe to reference current-1 (the previous time guarantee
 * is because if the rrd is empty, the value is simply stored, and we do
 * NOT take this path). This is used because the txg for this interval is
 * NOT the current -- it is the previous, because we want to err on the side
 * of earlier txg, not later.
 *
 * Missed update periods will result in the same txg pair put into the rrd
 * multiple times. This is correct, because we have no other data for the
 * period.This is as if we "smeared" ouselves and is neded to guarantee
 * constant time lookup.
 */
static void
txg_zero(rrd_t *r, void *p)
{
    int n;
    void *v;

    /*
     * There is at least one element in the ring buffer! So tail-1 makes
     * sense. We may want to assert(tail >= 0) to catch the empty case.
     * If tail == 0, the last element is at the end (capacity-1)
     */
    if (rrd_tail(r) == 0)
        n = rrd_capacity(r) - 1;
    else
        n = rrd_tail(r) - 1;
    v = rrd_entry(r, n);
    /*
     * And store that last element at tail (as the new element). Yes,
     * we call it "zero" and it does not zero -- this initializes the
     * element.
     */
    rrd_store(r, v);
}

void
txg_add_at(rrd_t *h, uint64_t v, struct timeval *tv)
{
	txg_store_t s;
	s.l = s.h = v;
	dbrrd_add_at(h, &s, tv);
}

/*
 * txg1 test
 *
 * This test knows a bit too much about the internals. It will
 * test the "smearing" into minute, day and year. We are keeping
 * 10 years,  365 days, and 1440 minutes.
 */
void
txg1(rrd_t *h)
{
	struct timeval tv = { 0, 0};
	struct timeval res;
	uint64_t txg = 0;
	int i, r;
	txg_store_t *ptxg;
	void *p;

	/*
	 * First, we are going to enter 1 minute 0..59 seconds. This
	 * should give us 1..60 spread of txg for minute, day and year.
	 * Our txgs begin at 1 and monotonically ascend.
	 */
	for (i = 0; i < 60; ++i) {
		tv.tv_sec = i;
		txg_add_at(h, ++txg, &tv);
	}
	/*
	 * Query midpoint of the minute... 30 seconds.
	 */
	tv.tv_sec = 30;
	r = dbrrd_query(h, &tv, &p, &res);
	if (r == 0) {
		fprintf(stderr, "txg1: query at 30 seconds failed\n");
		exit(EXIT_FAILURE);
	}
	ptxg = (txg_store_t *)p;
	fprintf(stderr, "query at 30 seconds\n");
	fprintf(stderr, "  res: %ld\n", res.tv_sec);
	fprintf(stderr, "  l: %lu h: %lu\n", ptxg->l, ptxg->h);
	/*
	 * Fill in 60..LIMIT
	 * 60 seconds per minute,
	 * 1440 seconds per day
	 * 365 days per year
	 * Fill in 11 years
	 */
#undef LIMIT
#define LIMIT (60 * 1440 * 365 * 11)
	fprintf(stderr, "filling in %lu seconds\n", LIMIT);
	/*
	 * On my Thinpad 460, 346 896 000 samples are recorded in
	 * 18.6 seconds. This is 18 650 332 samples per second
	 *
	 * As this represents 11 years of txg generation, at one
	 * txg per second, 18.6 seconds appears reasonable.
	 */
	for (i = 60; i < LIMIT; ++i) {
		tv.tv_sec = i;
		txg_add_at(h, ++txg, &tv);
	}
	/*
	 * We will now issue queries:
	 * 1 second in the future (should fail),
	 * 30 seconds in the past (should be retrieved from 60 second rrd)
	 * 86400 seconds in the past (retrieve from day)
	 * 31536000 seconds in the past (retrieve from years)
	 */
	tv.tv_sec = LIMIT + 1; /* 1 second in future */
	r = dbrrd_query(h, &tv, &p, &res);
	if (r == 1) {
		fprintf(stderr, "txg1: at future worked?\n");
		exit(EXIT_FAILURE);
	}

	tv.tv_sec = LIMIT - 30; /* 30 seconds in the past */
	r = dbrrd_query(h, &tv, &p, &res);
	if (r == 0) {
		fprintf(stderr, "txg1: at 30 seconds in the past failed?\n");
		exit(EXIT_FAILURE);
	}
	ptxg = (txg_store_t *)p;
	fprintf(stderr, "query at 30 seconds in the past\n");
	fprintf(stderr, "  res: %ld\n", res.tv_sec);
	fprintf(stderr, "  l: %lu h: %lu\n", ptxg->l, ptxg->h);

	tv.tv_sec = LIMIT - 86400 - 30; /* One day in the past */
	r = dbrrd_query(h, &tv, &p, &res);
	if (r == 0) {
		fprintf(stderr, "txg1: at 1 day in the past failed?\n");
		exit(EXIT_FAILURE);
	}
	ptxg = (txg_store_t *)p;
	fprintf(stderr, "query at 1 day in the past\n");
	fprintf(stderr, "  res: %ld\n", res.tv_sec);
	fprintf(stderr, "  l: %lu h: %lu\n", ptxg->l, ptxg->h);

	tv.tv_sec = LIMIT - 31536000 - 30; /* One year in the past */
	r = dbrrd_query(h, &tv, &p, &res);
	if (r == 0) {
		fprintf(stderr, "txg1: at 1 year in the past failed?\n");
		exit(EXIT_FAILURE);
	}
	ptxg = (txg_store_t *)p;
	fprintf(stderr, "query at 1 year in the past\n");
	fprintf(stderr, "  res: %ld\n", res.tv_sec);
	fprintf(stderr, "  l: %lu h: %lu\n", ptxg->l, ptxg->h);

	tv.tv_sec = LIMIT - (11*31536000) - 30; /* 11 years in the past */
	r = dbrrd_query(h, &tv, &p, &res);
	if (r == 1) {
		fprintf(stderr, "txg1: 11 years should be aged out?\n");
		exit(EXIT_FAILURE);
	}
}

void
txg_test(void)
{
	rrd_t *h;

	/*
	 * Must be sorted descending by timeval
	 *
	 * These are the period definitions
	 *
	 *       60 seconds per minute
	 *     3600 seconds per hour
	 *    86400 seconds per day
	 * 31536000 seconds per year
	 *     1440 minutes per day
	 *
	 * So, this keeps one day of txg at 1 minute resolution,
	 * one year of txg at 1 day resolution, and 10 years of
	 * txg at 1 year resolution.
	 * 
	 * Space taken is under 20K bytes.
	 */
	dbrrd_spec_t dbrrd_periods[] = {
		{   10, { 31536000, 0 } }, /* 10 years */
		{  365, {    86400, 0 } }, /* 365 days - 1 year in day brackets */
		{ 1440, {       60, 0 } }, /* 1440 minutes - 1 day in minute brackets */
		{ 0, { 0, 0 } }, 
	};

	fprintf(stderr,"txg_test\n");
	h = dbrrd_create("txg", dbrrd_periods, sizeof(txg_store_t),
		txg_update, txg_zero);
	/*
	 * FIXME: we are not actually filling or querying yet.
	 * Need to add the test vector.
	 */
	txg1(h);
	dbrrd_destroy(h);
	fprintf(stderr,"txg_test complete\n");
}

int
main(int ac, char **av)
{
	printf("crrd - C RRD Database\n");
	period_test();
	simple_test();
	complex_test();
	dbrrd_test();
	txg_test();
	return (EXIT_SUCCESS);
}

