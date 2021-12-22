#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>
#include "bsd-tree.h"

/* void * malloc(size_t size); */
/* void   free(void *ptr); */
/* void * calloc(size_t nmemb, size_t size); */
/* void * realloc(void *ptr, size_t size); */
/* void * reallocarray(void *ptr, size_t nmemb, size_t size); */


struct mo_chunk {
	RB_ENTRY(mo_chunk)  entry;
	int                 used;
	struct {
		uintptr_t		ptr;
		unsigned		pages;
	} aligned;
	struct {
		uintptr_t       ptr;
		const char *	info[2];
	} assigned;
};

static int
chunk_ptr_cmp(struct mo_chunk * p1, struct mo_chunk * p2)
{
	if (p1->assigned.ptr < p2->assigned.ptr) return -1;
	if (p1->assigned.ptr > p2->assigned.ptr) return  1;
	return 0;
}

RB_HEAD(mo_chunk_ptrtree, mo_chunk);
RB_PROTOTYPE(mo_chunk_ptrtree, mo_chunk, entry, chunk_ptr_cmp);
RB_GENERATE(mo_chunk_ptrtree, mo_chunk, entry, chunk_ptr_cmp);

struct mo_ctx {
	pthread_mutex_t mutex;
	struct mo_chunk_ptrtree ptr_tree;
};

static struct mo_ctx mo_ctx = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.ptr_tree = RB_INITIALIZER(NULL),
};

static struct mo_chunk *
mo_chunk_alloc(size_t size, const char * info1, const char * info2)
{
	int rc;

	assert(size > 0);
	// align to 4 bytes(int)
	size = (size + 3) & ~3U;
	size_t pages = 1 + (unsigned)(size + 4095U) / 4096U;

	void * ptr = mmap(NULL,
					  pages*4096,
					  PROT_READ|PROT_WRITE,
					  MAP_PRIVATE|MAP_ANONYMOUS,
					  -1,
					  0);
	assert(ptr != MAP_FAILED);
	if (ptr == MAP_FAILED) {
		return NULL;
	}
	int off = (pages-1)*4096 - size;
	struct mo_chunk * chunk = ptr + (pages-1)*4096;
	chunk->aligned.ptr		= (uintptr_t)ptr;
	chunk->aligned.pages	= pages;
	chunk->assigned.ptr		= (uintptr_t)ptr + off;
	chunk->assigned.info[0] = info1;
	chunk->assigned.info[1] = info2;

	rc = mprotect(chunk, 4096, PROT_NONE);
	assert(rc == 0);

	for(rc=0; rc<off; rc++) {
		*(char *)(ptr+rc) = 0;
	}
	return chunk->assigned.ptr;
}

static void
mo_chunk_free(struct mo_chunk * chunk)
{
	assert(chunk);
	munmap(chunk->aligned.ptr, chunk->aligned.pages*4096);
}

static struct mo_chunk *
mo_chunk_find(void * ptr)
{
	int rc = 0;
	struct mo_chunk f, * r;
	f.assigned.ptr = ptr;

	rc = pthread_mutex_lock(&mo_ctx.mutex);
	assert(rc == 0);
	r = RB_FIND(mo_chunk_ptrtree, &mo_ctx.ptr_tree, &f);
	rc = pthread_mutex_unlock(&mo_ctx.mutex);
	assert(rc == 0);

	return r;
}

static void *
mo_malloc(size_t size, const char * info1, const char * info2)
{
	int rc;

	struct mo_chunk * chunk = mo_chunk_alloc(size, info1, info2);
	if (!chunk) {
		return NULL;
	}
	rc = pthread_mutex_lock(&mo_ctx.mutex);
	assert(rc == 0);
	RB_INSERT(mo_chunk_ptrtree, &mo_ctx.ptr_tree, chunk);
	rc = pthread_mutex_unlock(&mo_ctx.mutex);
	assert(rc == 0);
	chunk->used = 1;

	return chunk->assigned.ptr;
}

static void
mo_free(void * ptr)
{
	struct mo_chunk * chunk = mo_chunk_find(ptr);

	if (!ptr) {
		assert(0 && "unable to find the ptr");
		return;
	}
	int rc = pthread_mutex_lock(&mo_ctx.mutex);
	assert(rc == 0);
	RB_REMOVE(mo_chunk_ptrtree, &mo_ctx.ptr_tree, chunk);
	rc = pthread_mutex_unlock(&mo_ctx.mutex);

	mo_chunk_free(chunk);
}



int main() {
	size_t i, size = 12;
	{
		char * ptr = (char *)mo_malloc(size, __FUNCTION__, __LINE__);
		mo_free(ptr);
	}

	{
		char * ptr = (char *)mo_malloc(size, __FUNCTION__, __LINE__);
		for(i=0; i<size; i++){
			printf(": %c ", ptr[i]);
		}
		mo_free(ptr);
	}

	return 0;
}
