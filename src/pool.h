#ifndef __ALLOCATOR_FIX_H
#define __ALLOCATOR_FIX_H

// complete binary tree
struct cbt {
	int order;
	int * nodes;  
};

struct pool {
	unsigned   size;     // total memory in bytes
	unsigned   esize;    // one element size in bytes
	unsigned   ecount;   // element count. 2^(k+5)
	unsigned * bit_array;// bits
	void     * element;   // element
	unsigned   used;     // remove it once CBT is enabled.
	struct cbt cbt;
};

struct pool * pool_init  (unsigned order, unsigned esize);
void   pool_fini  (struct pool *);
void * pool_alloc (struct pool *);
int    pool_free  (struct pool *, void *ptr);

#endif
