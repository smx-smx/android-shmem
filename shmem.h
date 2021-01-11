#ifndef __ANDROID_SHMEM_H
#define __ANDROID_SHMEM_H

#include <sys/types.h>

#define SOCKNAME "/dev/shm/%08x"
#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

typedef struct {
	int id;
	void *addr;
	int descriptor;
	size_t size;
	int markedForDeletion;
} shmem_t;

typedef struct {
	int sock;
	int sockid;
} shmem_ctx_t;

#endif /* __ANDROID_SHMEM_H */