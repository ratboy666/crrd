/*
 * crrd.h
 */

#ifdef TESTING
#include <stdint.h>
#include <sys/time.h>
typedef long long longlong_t;
typedef longlong_t hrtime_t;
#endif

typedef struct rrd {
	char *name;	      /* name */
	size_t asize;         /* allocation size */
	hrtime_t resolution;  /* time between successive entries */
	int capacity;	      /* capacity of database */
	size_t size;	      /* size of an entry */
	int head;	      /* head (beginning) */
	int tail;	      /* tail (end) */
	hrtime_t start;	      /* begin time of current bucket */
	hrtime_t last;	      /* last update time */
	struct rrd *next;     /* allow for list of rrd */
	void (*zero)(struct rrd *, void *);
	void (*update)(struct rrd *, void *);
	/*
	 * Ring buffer entries. Sized one uint64_t larger than is
	 * actually needed (capacity * size) bytesa. As soon as
	 * all C compilers we expect to work with this are updated,
	 * the [1] can change to [0].
	 *
	 * For maximum alignment, we declare this longlong_t (128 bit)
	 */
	longlong_t entries[1];
} rrd_t;

typedef struct dbrrd_spec {
	int capacity;
	hrtime_t tv;
} dbrrd_spec_t;

rrd_t *rrd_create(char *s, hrtime_t res, unsigned cap, size_t sz);
unsigned rrd_len(rrd_t *r);
hrtime_t rrd_resolution(rrd_t *r);
int rrd_capacity(rrd_t *r);
void rrd_debug(rrd_t *r);
void rrd_destroy(rrd_t *r);
void rrd_add_at(rrd_t *r, void *v, hrtime_t t);
void *rrd_entry(rrd_t *r, int i);
void *rrd_get(rrd_t *r, int i);
void rrd_add(rrd_t *r, void *v);
void rrd_setfunctions(rrd_t *r, void *fupdate, void *fzero);
int rrd_tail(rrd_t *r);

int dbrrd_query(rrd_t *r, hrtime_t tv, void **vp, hrtime_t *res);
void dbrrd_add_at(rrd_t *r, void *vp, hrtime_t t);
void dbrrd_add(rrd_t *r, void *v);
void dbrrd_destroy(rrd_t *h);
rrd_t *dbrrd_create(char *name, dbrrd_spec_t *p, size_t sz,
	void *update, void *zero);
