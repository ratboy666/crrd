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
	void *entries;	    /* pointer to entries */
	int head;	    /* head (beginning) */
	int tail;	    /* tail (end) */
	rrdt_t start;	    /* begin time of current bucket */
	rrdt_t last;	    /* last update time */
	struct rrd *next;   /* allow for list of rrd */
	void (*zero)(struct rrd *);
	void (*update)(struct rrd *, void *);
} rrd_t;

rrdt_t tv2rrdt(struct timeval *tv);
void rrdt2tv(struct timeval *tv, rrdt_t rt);
rrd_t *rrd_create(char *s, struct timeval *res, unsigned cap, size_t sz);
unsigned rrd_len(rrd_t *r);
rrdt_t rrd_resolution(rrd_t *r);
int rrd_capacity(rrd_t *r);
void rrd_debug(rrd_t *r);
void rrd_destroy(rrd_t *r);
void rrd_add_at(rrd_t *r, void *v, struct timeval *tv);
void *rrd_get(rrd_t *r, int i);
void rrd_add(rrd_t *r, void *v);
void rrd_setfunctions(rrd_t *r, void *fupdate, void *fzero);

