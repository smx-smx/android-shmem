#ifndef __SHMEM_SOCKET_H
#define __SHMEM_SOCKET_H

#include "shmem.h"

int get_shmid(shmem_ctx_t *ctx, unsigned int index);
int get_sockid(int shmid);
unsigned int get_index(int shmid);

#endif /* __SHMEM_SOCKET_H */