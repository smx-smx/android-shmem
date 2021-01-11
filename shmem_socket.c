#include "shmem_socket.h"

int get_shmid(shmem_ctx_t *ctx, unsigned int index) {
	return ctx->sockid * 0x10000 + index;
}

int get_sockid(int shmid) {
	return shmid / 0x10000;
}

unsigned int get_index(int shmid) {
	return shmid % 0x10000;
}