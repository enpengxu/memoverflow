#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>

struct pool * pool_init  (unsigned order);
void   pool_fini  (struct pool *);
void * pool_alloc (struct pool *);
void   pool_free  (struct pool *, void *ptr);


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

	esize = (esize + 3) & 3;
	unsigned cbt_off   = sizeof(struct pool);
	unsigned cbt_size  = sizeof(unsigned) * ((1<<(k+1)) -1);
	unsigned bits_off  = cbt_off + cbt_size;
	unsigned bits_size = sizeof(unsigned) * (1<<k);
	unsigned ele_off   = bits_off + bits_size;

	
	size_t bytes = sizeof(struct pool) +
		cbt_size + bits_size +  esize * ( 1 << order);
		
	void * ptr = mmap(NULL,
					  bytes,
					  PROT_READ|PROT_WRITE,
					  MAP_PRIVATE|MAP_ANONYMOUS,
					  -1,
					  0);
	assert(ptr);
	if (ptr == MAP_FAILED) {
		log_error("mmap failed. order: %u , element size: %u, total bytes: %u \n",
				  order, esize, bytes);
		return -1;
	}
	struct pool * pool = ptr;
	pool->size		= bytes;
	pool->esize		= esize;
	pool->ecount	= 1 << order;
	pool->bit_array = ptr + bits_off;
	pool->element	= ptr + ele_off;
	pool->cbt.order	= k;
	pool->cbt.nodes	= ptr + cbt_off;
	
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
	int idx = cbt_alloc(&pool->cbt);
	if (idx == -1) { // failed.
		log_error("Failed in pool_alloc. too many allocations. try to increase pool size.\n");
		return NULL;
	}
	unsigned *bit = &pool->bit_array[idx];
	assert(*bit != ~0U);

	int n = 0;
	while ((*bit) & (1<<n)) {
		n ++;
	}
	assert(n>=0 && n<32);
	*bit |= (1 << n);

	unsigned off = idx * 32 + n;
	return pool->element + off * esize;
}

void
pool_free(struct pool *pool, void * e)
{
	if (e < pool->element ||
		e >= (pool->element + ecount * esize)) {
		assert(0 && "pool_free: element overflow");
		log_error("Failed in pool_free. \n");
		return ;
	}
		
	int idx = (e - pool->element) / esize;
	assert(idx >=0 && idx < ecount);

	int cbt_idx = idx / 32;
	cbt_free(cbt_idx);

	pool->bit_array[cbt_idx] &= ~(1<< (idx % 32));
}




