#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>
#include "pool.h"

/* TODO */
#define log_error(...)

/* TODO */
#ifdef CBT_ENABLE
static int
cbt_alloc(struct cbt * cbt)
{
	int total = 1 << cbt->order;
	assert(cbt->nodes[0] <= total);

	if (cbt->nodes[0] == total)
		return -1;
	unsigned len = 1;
	unsigned * node = cbt->nodes;
	for(int i=1; i<order; i++) {
		node[0] ++;
		if (node[len] < total) {
			node += len; 
		} else {
			assert(node[len+1] < total);
			node += len + 1;
		}
		len = len * 2;
	}
	return node - (cbt->nodes + total);
}

static int
cbt_free(struct cbt * cbt, int idx)
{
	int total = 1 << cbt->order;
	assert(cbt->nodes[0] <= total);

	if (cbt->nodes[0] == total)
		return -1;

	unsigned len = total;
	unsigned * node = cbt->nodes;
	for(int i=order-1; i >= 0; i--) {
		node[0] --;
		len = len / 2;
		node -= idx/2;
	}
	return node - (cbt->nodes + total);
}

#endif

static unsigned
_pool_alloc(struct pool * pool)
{
	unsigned * p = pool->bit_array;
	unsigned i, n = pool->ecount >> 5;

	if (pool->used == pool->ecount)
		return -1U;

	for (i=0; i<n; i++) {
		if (p[0] != ~0U)
			break;
		p ++;
	}
	assert(*p != ~0U);
	n = i;
	for (i=0; i<32; i++) {
		if (!(p[0] & (1U<<i)))
			break;
	}
	assert(!(p[0] & (1U<<i)));
	p[0] |= (1U << i);
	pool->used ++;

	unsigned idx = (n << 5) + i;
	assert(pool->bit_array[idx/32] & (1U << i));
	return idx;
}

static int
_pool_free(struct pool * pool, unsigned idx)
{
	assert(idx < pool->ecount);
	unsigned   x1 = idx / 32;
	unsigned   x2 = idx % 32;
	unsigned * p = &pool->bit_array[x1];

	if (!(p[0] & (1U << x2))) {
		assert(0 && "invalid index in _pool_free");
		return -1;
	}
	assert(p[0] & (1U << x2));
	p[0] &= ~(1U << x2);
	pool->used --;
	return 0;
}


void
pool_reset(struct pool * pool)
{
	pool->used = 0;
	memset(&pool->bit_array[0], 0, sizeof(unsigned)*pool->ecount/32);
}

// memory layout
// k = order - 5
// 1. struct pool
// 2. complete binary tree: 4 * (2^(k+1) -1)
// 3. 32 bits mask array: 4 * (2^k)
// 4. memory (element_size * 2^order)
//
struct pool *
pool_init  (unsigned order, unsigned esize)
{
	if (order < 5)
		order = 5;
	int k = order - 5;
	esize = (esize + 3U) & ~3U;

#ifdef CBT_ENABLE
	unsigned cbt_off   = sizeof(struct pool);
	unsigned cbt_size  = sizeof(unsigned) * ((1<<(k+1)) -1);
#else
	unsigned cbt_off   = sizeof(struct pool);
	unsigned cbt_size  = 0;
#endif	
	unsigned bits_off  = cbt_off + cbt_size;
	unsigned bits_size = sizeof(unsigned) * (1U<<k);
	unsigned elem_off   = bits_off + bits_size;
	
	size_t bytes = sizeof(struct pool) +
		cbt_size + bits_size +  esize * (1 << order);
		
	void * ptr = mmap(NULL,
					  bytes,
					  PROT_READ|PROT_WRITE,
					  MAP_PRIVATE|MAP_ANONYMOUS,
					  -1,
					  0);
	assert(ptr);
	if (ptr == MAP_FAILED) {
		assert(0 && "mmap failed");
		log_error("mmap failed. order: %u , element size: %u, total bytes: %u \n",
				  order, esize, bytes);
		return NULL;
	}
	struct pool * pool = ptr;
	pool->size		= bytes;
	pool->esize		= esize;
	pool->ecount	= 1U << order;
	pool->bit_array = ptr + bits_off;
	pool->element	= ptr + elem_off;
	pool->cbt.order	= k;
	pool->cbt.nodes	= ptr + cbt_off;

	pool_reset(pool);
	return pool;
}

void 
pool_fini (struct pool * pool)
{
	assert(pool && pool->size);
	munmap(pool, pool->size);
}

void *
pool_alloc(struct pool *pool)
{
#ifdef CBT_ENABLE	
	unsigned idx = cbt_alloc(&pool->cbt);
#else
	unsigned idx = _pool_alloc(pool);
#endif	
	if (idx == -1U) { // failed.
		log_error("Failed in pool_alloc. too many allocations. try to increase pool size.\n");
		return NULL;
	}
	return pool->element + idx * pool->esize;
}

int
pool_free(struct pool *pool, void * e)
{
	if (e < pool->element ||
		e >= (pool->element + pool->ecount * pool->esize)) {
		assert(0 && "pool_free: element overflow");
		log_error("Failed in pool_free. \n");
		return -1;
	}		
	unsigned idx = (e - pool->element) / pool->esize;
	assert(idx >=0 && idx < pool->ecount);

#ifdef CBT_ENABLE
	int cbt_idx = idx / 32;
	cbt_free(cbt_idx);

	pool->bit_array[cbt_idx] &= ~(1<< (idx % 32));
#else
	return _pool_free(pool, idx);
#endif	
}

#ifdef POOL_TEST

static void
pool_test()
{
	unsigned i, o, e, n, idx;
	struct pool * pool;
	{
		// 1024, 4, 256 total
		o = 5; //10;
		e = 4;
		n = 1U << o;
	}
	pool = pool_init(o, e);
	assert(pool);
	void ** ptr = malloc(sizeof(void **)*n);

	for (int m=0; m<1000; m++){
		for (i=0; i<n; i++) {
			ptr[i] = pool_alloc(pool);
			idx = (ptr[i] - pool->element) / e;
			assert(i == idx);
			*(int *)ptr[i] = i;
		}
		void * p = pool_alloc(pool);
		assert(p == NULL);
		assert(pool->used == pool->ecount);
		for (i=0; i<n; i++) {
			pool_free(pool, ptr[i]);
		}
		assert(pool->used == 0);

		for (i=0; i<n; i++) {
			ptr[i] = pool_alloc(pool);
			idx = (ptr[i] - pool->element) / e;
			assert(i == idx);
			assert(*(int *)ptr[i] == i);

		}
		for (i=0; i<n; i++) {
			pool_free(pool, ptr[i]);
			ptr[i] = NULL;
		}
		assert(pool->used == 0);
		pool_reset(pool);
	}	
	printf("pool: test : passed basic test\n");

	
	int loop = 100000000;
	do {
		memset(ptr, 0, sizeof(void **)*n);
		pool_reset(pool);

		while(pool->used < pool->ecount) {
			int r = random() % n;
			if (!ptr[r])
				ptr[r] = pool_alloc(pool);
			else {
			    pool_free(pool, ptr[r]);
				ptr[r] = NULL;
			}

			for (int k=0; k<5; k++) {
				r = random() % n;
				if (!ptr[r])
					ptr[r] = pool_alloc(pool);
			}
		}
		loop --;
	} while(loop == 0);

	printf("pool: test : passed stressed test\n");

	
	pool_fini(pool);
}


int main(){
	pool_test();

	return 0;
}

#endif
