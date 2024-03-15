/*
 * crrd.h
 */

#include <stdint.h>
#include <sys/time.h>

/*
 * Internal times in the rrd are maintained as rrdt_t, which is in
 * seconds, milliseconds, microseconds... actual unit is dependent
 * on the system/usecase.
 */
typedef uint64_t rrdt_t;

typedef struct rrd {
	char *name;	    /* name */
	rrdt_t resolution;  /* time between successive entries */
	int capacity;	    /* capacity of database */
	size_t size;	    /* size of an entry */
	int head;	    /* head (beginning) */
	int tail;	    /* tail (end) */
	rrdt_t start;	    /* begin time of current bucket */
	rrdt_t last;	    /* last update time */
	struct rrd *next;   /* allow for list of rrd */
	void (*zero)(struct rrd *, void *);
	void (*update)(struct rrd *, void *);
	/*
	 * Ring buffer entries. Sized one uint64_t larger than is
	 * actually needed (capacity * size) bytesa. As soon as
	 * all C compilers we expect to work with this are updated,
	 * the [1] can change to [0].
	 */
	uint64_t entries[1];
} rrd_t;

typedef struct dbrrd_spec {
	int capacity;
	struct timeval tv;
} dbrrd_spec_t;

rrdt_t tv2rrdt(struct timeval *tv);
void rrdt2tv(struct timeval *tv, rrdt_t rt);

rrd_t *rrd_create(char *s, struct timeval *res, unsigned cap, size_t sz);
unsigned rrd_len(rrd_t *r);
rrdt_t rrd_resolution(rrd_t *r);
int rrd_capacity(rrd_t *r);
void rrd_debug(rrd_t *r);
void rrd_destroy(rrd_t *r);
void rrd_add_at(rrd_t *r, void *v, struct timeval *tv);
void *rrd_entry(rrd_t *r, int i);
void *rrd_get(rrd_t *r, int i);
void rrd_add(rrd_t *r, void *v);
void rrd_setfunctions(rrd_t *r, void *fupdate, void *fzero);
int rrd_tail(rrd_t *r);

int dbrrd_query(rrd_t *r, struct timeval *tv, void **vp, struct timeval *res);
void dbrrd_add_at(rrd_t *r, void *vp, struct timeval *t);
void dbrrd_add(rrd_t *r, void *v);
void dbrrd_destroy(rrd_t *h);
rrd_t *dbrrd_create(char *name, dbrrd_spec_t *p, size_t sz,
	void *update, void *zero);
