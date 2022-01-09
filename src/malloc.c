#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include "bsd-tree.h"
#include "pool.h"

struct mo_rbnode {
	RB_ENTRY(mo_rbnode)  ptr_entry;
	RB_ENTRY(mo_rbnode)  page_entry;
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
mo_rbnode_ptr_cmp(struct mo_rbnode * p1, struct mo_rbnode * p2)
{
	if (p1->user.ptr < p2->user.ptr) return -1;
	if (p1->user.ptr > p2->user.ptr) return  1;
	return 0;
}

static int
mo_rbnode_page_cmp(struct mo_rbnode * p1, struct mo_rbnode * p2)
{
	if (p1->pages.num < p2->pages.num) return -1;
	if (p1->pages.num > p2->pages.num) return  1;
	return 0;
}

RB_HEAD(mo_rbnode_ptr_tree, mo_rbnode);
RB_PROTOTYPE(mo_rbnode_ptr_tree, mo_rbnode, ptr_entry, mo_rbnode_ptr_cmp);
RB_GENERATE (mo_rbnode_ptr_tree, mo_rbnode, ptr_entry, mo_rbnode_ptr_cmp);

RB_HEAD(mo_rbnode_pages_tree, mo_rbnode);
RB_PROTOTYPE(mo_rbnode_pages_tree, mo_rbnode, page_entry, mo_rbnode_page_cmp);
RB_GENERATE (mo_rbnode_pages_tree, mo_rbnode, page_entry, mo_rbnode_page_cmp);


struct mo_ctx {
	int             reuse_memory;
	pthread_mutex_t pool_mutex;
	struct pool   * pool;

	struct {
		pthread_mutex_t             mutex;
		struct mo_rbnode_ptr_tree   tree;
	} ptr_tree;
	struct {
		pthread_mutex_t             mutex;
		struct mo_rbnode_pages_tree tree;
	} page_tree;
};


static struct mo_ctx mo_ctx = {
	.reuse_memory = 1,
	.pool       = NULL,
	.pool_mutex = PTHREAD_MUTEX_INITIALIZER,

	.ptr_tree = {
		.mutex  = PTHREAD_MUTEX_INITIALIZER,
		.tree   = RB_INITIALIZER(NULL),
	},
	.page_tree = {
		.mutex  = PTHREAD_MUTEX_INITIALIZER,
		.tree   = RB_INITIALIZER(NULL),
	},
};

static pthread_once_t mo_once = PTHREAD_ONCE_INIT;
static size_t sys_pagesize = 4096;

static void
mo_init(void)
{
	unsigned order = 20;
	char * env = getenv("MO_POOL_ORDER");
	if (env) {
		order = atoi(env);
	}
	if (order <= 0) order = 20;

	env = getenv("MO_REUSE_MEM");
	if (env) {
		mo_ctx.reuse_memory = atoi(env);
	}
	
	mo_ctx.pool = pool_init(order, sizeof(struct mo_rbnode));
	assert(mo_ctx.pool);

	sys_pagesize = (size_t)sysconf(_SC_PAGESIZE);
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
	void * ptr;
	ptr = mmap(NULL,
			   (1+pages)*sys_pagesize,
			   PROT_READ|PROT_WRITE,
			   MAP_PRIVATE|MAP_ANONYMOUS,
			   -1, //NOFD,
			   0);
	assert(ptr != MAP_FAILED);
	if (ptr == MAP_FAILED) {
		return NULL;
	}
	rc = mprotect(ptr+pages*sys_pagesize, sys_pagesize, PROT_NONE);
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
	rc = munmap((void *)node->pages.ptr, node->pages.num * sys_pagesize);
	if (rc) {
		fprintf(stderr, "munmap failed. errno=%d \n", errno);
	}
	return rc;
}

#define mo_node_find_remove(_tree, key)		\
	({											\
		int rc = 0; \
		struct mo_rbnode f, * r;								 \
		f.user.ptr = (uintptr_t)ptr;							 \
		rc = pthread_mutex_lock(&(_tree).mutex);				 \
		assert(rc == 0);										 \
		r = RB_FIND(mo_rbnode_ptr_tree, &(_tree).tree, &f);		 \
		if (r)													 \
			RB_REMOVE(mo_rbnode_ptr_tree, &(_tree).tree, r);	 \
		rc = pthread_mutex_unlock(&(_tree).mutex);				 \
		assert(rc == 0);										 \
		r;														 \
	})


static struct mo_rbnode *
mo_rbnode_find(void * ptr, int remove)
{
	//return mo_node_find_remove(mo_ctx.ptr_tree, ptr);
		
	int rc = 0;
	struct mo_rbnode f, * r;
	f.user.ptr = (uintptr_t)ptr;

	rc = pthread_mutex_lock(&mo_ctx.ptr_tree.mutex);
	assert(rc == 0);
	r = RB_FIND(mo_rbnode_ptr_tree, &mo_ctx.ptr_tree.tree, &f);
	if (r && remove) {
		RB_REMOVE(mo_rbnode_ptr_tree, &mo_ctx.ptr_tree.tree, r);
	}
	rc = pthread_mutex_unlock(&mo_ctx.ptr_tree.mutex);
	assert(rc == 0);

	return r;
}

static struct mo_rbnode *
mo_pagetree_alloc(int pages)
{
	//return mo_node_find_remove(mo_ctx.page_tree, pages);

	int rc = 0;
	struct mo_rbnode f, * r;
	f.pages.num = pages;

	rc = pthread_mutex_lock(&mo_ctx.page_tree.mutex);
	assert(rc == 0);
	r = RB_FIND(mo_rbnode_pages_tree, &mo_ctx.page_tree.tree, &f);
	if (r) {
		RB_REMOVE(mo_rbnode_pages_tree, &mo_ctx.page_tree.tree, r);
	}
	rc = pthread_mutex_unlock(&mo_ctx.page_tree.mutex);
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

	// align to 4 bytes(int)
	assert(size > 0);
	size_t size1 = (size + 3) & ~3U;
	size_t pages = (unsigned)(size1 + sys_pagesize-1) / sys_pagesize;
	unsigned off = pages*sys_pagesize - size1;

	// try to alloc from pages_tree first.
	struct mo_rbnode * node;
	node = mo_pagetree_alloc(pages+1);
	if (node) {
		assert(node->pages.ptr && node->pages.num == (1 + pages));

		rc = mprotect((void *)node->pages.ptr, pages*sys_pagesize, PROT_READ|PROT_WRITE); 
		if (rc) {
			printf("error: failed in mprotect %d\n", errno);
			exit(-1);
		}
	} else {
		node = mo_rbnode_alloc();
		if (!node) {
			assert(0 && "unable alloc rbnode");
			return NULL;
		}
		void * ptr = mo_page_alloc(pages);

		node->pages.ptr   = (uintptr_t)ptr;
		node->pages.num  = 1 + pages;
	}
	node->user.ptr   = (uintptr_t)node->pages.ptr + off;
	node->user.info  = info;
	node->user.size  = size;

	rc = pthread_mutex_lock(&mo_ctx.ptr_tree.mutex);
	assert(rc == 0);
	RB_INSERT(mo_rbnode_ptr_tree, &mo_ctx.ptr_tree.tree, node);
	rc = pthread_mutex_unlock(&mo_ctx.ptr_tree.mutex);

	return (void *)node->user.ptr;
}

static void
mo_free(void * ptr)
{
	int rc;
	struct mo_rbnode * node;

	pthread_once(&mo_once, mo_init);
	assert(mo_ctx.pool);
	if (!mo_ctx.pool) {
		return ;
	}
	// find & remove
	node = mo_rbnode_find(ptr, 1);
	if (!node) {
		assert(0 && "unable to find the ptr");
		return;
	}

	if (mo_ctx.reuse_memory) {
		// try to reuse pages.
		// 1. insert node into pages tree
		rc = pthread_mutex_lock(&mo_ctx.page_tree.mutex);
		assert(rc == 0);
		RB_INSERT(mo_rbnode_pages_tree, &mo_ctx.page_tree.tree, node);
		rc = pthread_mutex_unlock(&mo_ctx.page_tree.mutex);
		assert(rc == 0);

		rc = mprotect((void *)node->pages.ptr, (node->pages.num-1)*sys_pagesize, PROT_NONE);
		assert(rc ==0);
		if (rc) {
			printf("mprotect failed: errno %d\n", errno); 
			exit(-1);
		}
		// 2. keep node in pool
	} else {
		rc = mo_page_free(node);
		assert(rc == 0);

		rc = mo_rbnode_free(node);
		assert(rc == 0);
	}
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
	struct mo_rbnode * node = mo_rbnode_find(p, 0);
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
