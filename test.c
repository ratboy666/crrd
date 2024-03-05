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
 */

#define _XOPEN_SOURCE
#define STANDALONE

#include "crrd.c"

void
period_test(void)
{
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
zero(rrd_t r)
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
	old = *(float *)(r->entries + (r->tail * r->size)); 
	res = r->resolution / 1000;
	new = old;
	new -= new / res;
	new += v / res;

	rrd_store(r, &new);
}

static void
f_zero(rrd_t *r)
{
    float z = 0.0;
    rrd_store(r, &z);
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

/*
 * The rrd_find function looks in the rrd for the time t. It returns
 * the value from the tightest period that contains the specified
 * time. Value and resolution are returned. Return is 1 if we have
 * data, 0 if not.
 *
 * The rrds are linked together -- most precise first, and ordered.
 * We find the first rrd that covers our time.
 */
int multi_find(rrd_t *r, struct timeval *tv, void **vp, struct timeval *res)
{
	rrdt_t t, t0, start;
	int i;

	t = tv2rrdt(tv);

	/* Find for time in future fails */
	if (t > r->last) {
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

		t0 = find_period(t, r->resolution);

		/*
		 * Time start for this rdd (may not be full). r->start
		 * is the start of the active period.
		 */
		start = r->start - (r->resolution * (rrd_len(r) - 1));

		/* Is the query time within the coverage of this rrd? */
		if (t0 >= start) {

			i = (t0 - start) / r->resolution;
			*vp = rrd_get(r, i);
			rrdt2tv(res, r->resolution);
			return (1);
		}

		/* Query time is out of this rrd, try next rrd (which
		 * is a bit "fuzzier".
		 */
	}

	/* Too old, no record */
	return (0);
}

void
multi_add_at(rrd_t *r, void *vp, struct timeval *t)
{
	while (r != NULL) {
	    rrd_add_at(r, vp, t);
	    r = r->next;
	}
}

void
multi_destroy(rrd_t *h)
{
	rrd_t *p;

	while (h != NULL) {
	    p = h->next;
	    fprintf(stderr, "freeing %s\n", h->name);
	    rrd_destroy(h);
	    h = p;
	}
}

rrd_t *
multi_create(void)
{
	rrd_t *h;
	rrd_t *r;
	struct timeval resolution = { 0, 0 };

	/*
	 * We are going to keep 100 1 seconds, 100 10 seconds,
	 * 100 100 seconds and 100 1000 seconds. This covers
	 * 1 day. (27.77 hours)
	 *
	 * Since rrd has a single next pointer, we create these
	 * in descending period size and link them.
	 */

	/* 1000 seconds */
	resolution.tv_sec = 1000;
	r = rrd_create("multi1000", &resolution, 100, sizeof (float));
	if (r == NULL) {
		fprintf(stderr, "rrd_create failed\n");
		exit(EXIT_FAILURE);
	}
	rrd_setfunctions(r, f_update, f_zero);
	h = r;

	/* 100 seconds */
	resolution.tv_sec = 100;
	r = rrd_create("multi100", &resolution, 100, sizeof (float));
	if (r == NULL) {
		fprintf(stderr, "rrd_create failed\n");
		exit(EXIT_FAILURE);
	}
	rrd_setfunctions(r, f_update, f_zero);
	r->next = h;
	h = r;

	/* 10 seconds */
	resolution.tv_sec = 10;
	r = rrd_create("multi10", &resolution, 100, sizeof (float));
	if (r == NULL) {
		fprintf(stderr, "rrd_create failed\n");
		exit(EXIT_FAILURE);
	}
	rrd_setfunctions(r, f_update, f_zero);
	r->next = h;
	h = r;

        /* 1 seconds */
	resolution.tv_sec = 1;
	r = rrd_create("multi1", &resolution, 100, sizeof (float));
	if (r == NULL) {
		fprintf(stderr, "rrd_create failed\n");
		exit(EXIT_FAILURE);
	}
	rrd_setfunctions(r, f_update, f_zero);
	r->next = h;
	h = r;

	/*
	 * multi1 -> multi10 -> multi100 -> multi1000 -> null
	 */
}

void
multi_test(void)
{
	rrd_t *h;
	struct timeval tv = { 0, 0 };
	struct timeval res;
	float v;
	int n;
	void *p;

	fprintf(stderr, "multi_test\n");
	h = multi_create();

	v = 5.0;
	tv.tv_sec = 0;
	multi_add_at(h, &v, &tv);

	v = 6.0;
	tv.tv_sec = 1;
	multi_add_at(h, &v, &tv);

	v = 7.0;
	tv.tv_sec = 2;
	multi_add_at(h, &v, &tv);

	n = multi_find(h, &tv, &p, &res);

	multi_destroy(h);
	fprintf(stderr, "multi_test complete\n");
}

int
main(int ac, char **av)
{
	printf("crrd - C RRD Database\n");
	period_test();
	simple_test();
	complex_test();
	multi_test();
	return (EXIT_SUCCESS);
}

