#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include "bsd-tree.h"
#include "pool.h"

/* void * malloc(size_t size); */
/* void   free(void *ptr); */
/* void * calloc(size_t nmemb, size_t size); */
/* void * realloc(void *ptr, size_t size); */
/* void * reallocarray(void *ptr, size_t nmemb, size_t size); */


struct mo_rbnode {
	RB_ENTRY(mo_rbnode)  entry;
	struct {
		uintptr_t		ptr;
		unsigned		num;
	} pages;
	struct {
		uintptr_t       ptr;
		size_t          size;
		const char *	info;
	} user;
};

static int
chunk_ptr_cmp(struct mo_rbnode * p1, struct mo_rbnode * p2)
{
	if (p1->user.ptr < p2->user.ptr) return -1;
	if (p1->user.ptr > p2->user.ptr) return  1;
	return 0;
}

RB_HEAD(mo_rbnode_ptrtree, mo_rbnode);
RB_PROTOTYPE(mo_rbnode_ptrtree, mo_rbnode, entry, chunk_ptr_cmp);
RB_GENERATE(mo_rbnode_ptrtree, mo_rbnode, entry, chunk_ptr_cmp);

struct mo_ctx {
	pthread_mutex_t rb_mutex;
	struct mo_rbnode_ptrtree ptr_tree;
	pthread_mutex_t pool_mutex;
	struct pool * pool;
};

static struct mo_ctx mo_ctx = {
	.rb_mutex = PTHREAD_MUTEX_INITIALIZER,
	.pool_mutex = PTHREAD_MUTEX_INITIALIZER,
	.ptr_tree = RB_INITIALIZER(NULL),
};

static pthread_once_t mo_once = PTHREAD_ONCE_INIT;

static void
mo_init(void)
{
	unsigned order = 20;
	char * env = getenv("MO_POOL_ORDER");
	if (env) {
		order = atoi(env);
	}
	if (order <= 0) order = 20;

	mo_ctx.pool = pool_init(order, sizeof(struct mo_rbnode));
	assert(mo_ctx.pool);
}

static struct mo_rbnode *
mo_rbnode_alloc()
{
	int rc;
	struct mo_rbnode * node;
	rc = pthread_mutex_lock(&mo_ctx.pool_mutex);
    assert(rc == 0);
	node = pool_alloc(mo_ctx.pool);
	rc = pthread_mutex_unlock(&mo_ctx.pool_mutex);
    assert(rc == 0);

	return node;
}
static int
mo_rbnode_free(struct mo_rbnode *node)
{
	int rc, r;
	rc = pthread_mutex_lock(&mo_ctx.pool_mutex);
    assert(rc == 0);
	r = pool_free(mo_ctx.pool, node);
    assert(r == 0);
	rc = pthread_mutex_unlock(&mo_ctx.pool_mutex);
    assert(rc == 0);

	return r;
}

static void *
mo_page_alloc(size_t pages)
{
	int rc;

	void * ptr = mmap(NULL,
					  (1+pages)*4096,
					  PROT_READ|PROT_WRITE,
					  MAP_PRIVATE|MAP_ANONYMOUS,
					  -1, //NOFD,
					  0);
	assert(ptr != MAP_FAILED);
	if (ptr == MAP_FAILED) {
		return NULL;
	}
	rc = mprotect(ptr+pages*4096, 4096, PROT_NONE); //PROT_READ|PROT_WRITE); //
	if (rc) {
		fprintf(stderr, "mprotect failed. errno=%d \n", errno);
		assert(0 && "mprotect failed");
	}
	assert(rc == 0);
	return ptr;
}

static int
mo_page_free(struct mo_rbnode * node)
{
	int rc;
	assert(node);
	/* rc = mprotect(node->pages.ptr + (node->pages.num-1)*4096, */
	/* 			  4096, PROT_READ|PROT_WRITE); */
	/* assert(rc == 0); */
	rc = munmap(node->pages.ptr, node->pages.num*4096);
	if (rc) {
		fprintf(stderr, "munmap failed. errno=%d \n", errno);
	}
}

static struct mo_rbnode *
mo_rbnode_find(void * ptr)
{
	int rc = 0;
	struct mo_rbnode f, * r;
	f.user.ptr = ptr;

	rc = pthread_mutex_lock(&mo_ctx.rb_mutex);
	assert(rc == 0);
	r = RB_FIND(mo_rbnode_ptrtree, &mo_ctx.ptr_tree, &f);
	rc = pthread_mutex_unlock(&mo_ctx.rb_mutex);
	assert(rc == 0);

	return r;
}

static void *
mo_malloc(size_t size, const char * info)
{
	int rc;
	pthread_once(&mo_once, mo_init);
	assert(mo_ctx.pool);
	if (!mo_ctx.pool) {
		return NULL;
	}
	struct mo_rbnode * node = mo_rbnode_alloc();
	if (!node) {
		assert(0 && "unable alloc rbnode");
		return NULL;
	}
	// align to 4 bytes(int)
	assert(size > 0);
	size_t size1 = (size + 3) & ~3U;
	size_t pages = (unsigned)(size1 + 4095U) / 4096U;

	void * ptr = mo_page_alloc(pages);
	unsigned off = pages*4096 - size1;

	node->pages.ptr	 = (uintptr_t)ptr;
	node->pages.num  = 1 + pages;
	node->user.ptr   = (uintptr_t)ptr + off;
	node->user.info  = info;
	node->user.size  = size;
	rc = pthread_mutex_lock(&mo_ctx.rb_mutex);
	assert(rc == 0);
	RB_INSERT(mo_rbnode_ptrtree, &mo_ctx.ptr_tree, node);
	rc = pthread_mutex_unlock(&mo_ctx.rb_mutex);

	return node->user.ptr;
}

static void
mo_free(void * ptr)
{
	struct mo_rbnode * node;

	pthread_once(&mo_once, mo_init);
	assert(mo_ctx.pool);
	if (!mo_ctx.pool) {
		return ;
	}

	node = mo_rbnode_find(ptr);
	if (!node) {
		assert(0 && "unable to find the ptr");
		return;
	}

	int rc = pthread_mutex_lock(&mo_ctx.rb_mutex);
	assert(rc == 0);
	RB_REMOVE(mo_rbnode_ptrtree, &mo_ctx.ptr_tree, node);
	rc = pthread_mutex_unlock(&mo_ctx.rb_mutex);

	rc = mo_page_free(node);
	assert(rc == 0);

	rc = mo_rbnode_free(node);
	assert(rc == 0);
}

void *
malloc(size_t size){
	if (size == 0) {
		return NULL;
	}
	return mo_malloc(size, NULL);
}

void
free(void *ptr) {
	if (!ptr) {
		return ;
	}
	return mo_free(ptr);
}

void *
calloc(size_t size, size_t n)
{
	void * ptr = mo_malloc(size*n, NULL);
	return ptr;
}

void *
realloc(void *p, size_t const nbytes)
{
	if (!p) {
		return mo_malloc(nbytes, NULL);
	}
	struct mo_rbnode * node = mo_rbnode_find(p);
	if (!node) {
		return NULL;
	}
	void * ptr = mo_malloc(nbytes, NULL);
	if (!ptr) {
		return NULL;
	}
	size_t size_cpy = node->user.size;
	if (size_cpy > nbytes)
		size_cpy = nbytes;

	memcpy(ptr, p, size_cpy);
	mo_free(p);

	return ptr;
}


#ifdef MALLOC_TEST

int main() {
	
	size_t i, size, NUM= 500000; //100000;
	char ** ptr;
	ptr = (char **)mo_malloc(sizeof(char *)*NUM, __FUNCTION__);
	assert(ptr);

	for (i=0; i<NUM; i++) {
		size = i+1;
		ptr[i] = (char *)mo_malloc(size, __FUNCTION__);
		//memset(ptr[i], 0, size);
		ptr[i][0] = 0;
		ptr[i][size-1] = 0;
		mo_free(ptr[i]);
		ptr[i] = NULL;
	}
	printf("mo: passed basic test\n");

	unsigned loop = 32;
	while(loop--) {
		for (i=0; i<NUM; i++) {
			size = 1 + random() % 1024*1024*2; // <= 2M
			ptr[i] = (char *)mo_malloc(size, __FUNCTION__);
			assert(ptr[i]);
			if (ptr[i]) {
				ptr[i][0] = 0;
				ptr[i][size-1] = 0;
			}
		#if 1	
			int k = random() % (i+1);
			if (ptr[k]) {
				mo_free(ptr[k]);
				ptr[k] = NULL;
			}
		#else
			mo_free(ptr[i]);
			ptr[i] = NULL;
		#endif	
		}
		for (i=0; i<NUM; i++) {
			if (ptr[i]) {
				mo_free(ptr[i]);
				ptr[i] = NULL;
			}
		}
		printf("mo: stress test %d is done\n", loop);

	}
	printf("mo: passed stress test\n");
	return 0;
}
#endif
