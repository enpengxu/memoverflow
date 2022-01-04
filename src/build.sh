gcc -fPIC  -g pool.c malloc.c -lpthread -shared -o libmozart.so
gcc -fPIC  -g pool.c malloc.c -lpthread -DMALLOC_TEST -o malloc_test
gcc -fPIC  -g pool.c -DPOOL_TEST -o pool_test


