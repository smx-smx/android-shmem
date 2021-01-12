#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>

#define STRINGIFY(x) STRINGIFY2(x)
#define STRINGIFY2(x) #x

#define LOG_PREFIX "[" __FILE__ ":" STRINGIFY(__LINE__) "] %s() : "

#define SUN_PATH_ABSTRACT(ptr) ((char *)(ptr) + offsetof(struct sockaddr_un, sun_path) + 1)
#define MAX_BIND_ATTEMPTS 4096

#if defined(__ANDROID__) && !defined(ASHMEM_STDOUT_LOGGING)
#include <android/log.h>
#include "sys/shm.h"

#ifdef NDEBUG
#define DBG(fmt, ...) do {} while (0)
#else
#define DBG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "shmem", LOG_PREFIX fmt, __func__, ##__VA_ARGS__)
#endif

#else /* __ANDROID__ */
#include <sys/shm.h>

#define DBG(fmt, ...) fprintf(stderr, LOG_PREFIX fmt "\n", __func__, ##__VA_ARGS__)
#endif /* __ANDROID__ */

#define UNUSED(x) (void)(x)

#include "libancillary/ancillary.h"
#include "cutils/ashmem.h"

#include "shmem.h"
#include "shmem_socket.h"

static pthread_t listening_thread_id = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static shmem_ctx_t gCtx;

static int shm_find_by_addr(shmem_ctx_t *ctx, const void *addr){
	unsigned int i;
	for (i=0; i<ctx->shmem_amount; i++){
		if(ctx->pool[i].addr == addr){
			return i;
		}
	}
	DBG ("cannot find addr %p", addr);
	return -1;
}

static int shm_find_by_key(shmem_ctx_t *ctx, key_t key){
	unsigned int i;
	for (i = 0; i < ctx->shmem_amount; i++) {
		if (ctx->pool[i].key == key){
			return i;
		}
	}
	DBG ("cannot find key %x", key);
	return -1;
}

static int shm_find_by_id(shmem_ctx_t *ctx, key_t shmid){
	unsigned int i;
	for (i = 0; i < ctx->shmem_amount; i++) {
		if (ctx->pool[i].id == shmid){
			return i;
		}
	}
	DBG ("cannot find shmid %x", shmid);
	return -1;
}

void *shmem_resize(shmem_ctx_t *ctx, int shmem_amount){
	ctx->shmem_amount = shmem_amount;
	ctx->pool = realloc(ctx->pool, ctx->shmem_amount * sizeof(shmem_t));
	return ctx->pool;
}

char *asun_build_path(key_t key){
	struct sockaddr_un dummy;
	int max_length = sizeof(dummy.sun_path) - 1;
	char *buf = calloc(max_length, 1);
	snprintf(buf, max_length, SOCKNAME, key);
	return buf;
}

int asun_path_cpy(char *dest, const char *src){
	struct sockaddr_un dummy;
	int in_length = strlen(src) + 1;
	int max_length = sizeof(dummy.sun_path) - 1;

	int length = in_length;
	if(in_length > max_length){
		length = max_length;
	}

	strncpy(dest, src, length);
	return length;
}

static void *listening_thread(void *arg) {
	DBG ("thread started");

	shmem_ctx_t *ctx = (shmem_ctx_t *)arg;
	shmem_t *pool = ctx->pool;

	struct sockaddr_un addr;
	socklen_t len = sizeof(addr);

	int client_sock = -1;
	while ((client_sock = accept(ctx->sock, (struct sockaddr *)&addr, &len)) != -1) {
		do {
			key_t key;
			if (recv (client_sock, &key, sizeof(key), 0) != sizeof(key)) {
				DBG ("ERROR: recv() returned not %d bytes", (int)sizeof(key));
				break;
			}

			pthread_mutex_lock (&mutex);
			{
				int idx = shm_find_by_key(ctx, key);
				if (idx != -1) {
					if (ancil_send_fd (client_sock, pool[idx].descriptor) != 0) {
						DBG ("ERROR: ancil_send_fd() failed: %s", strerror(errno));
					}
				} else {
					DBG ("ERROR: cannot find key 0x%x", key);
				}
			}
			pthread_mutex_unlock (&mutex);
		} while(0);

		close (client_sock);
	}

	DBG ("ERROR: listen() failed, thread stopped");
	return NULL;
}

static int create_listener(shmem_ctx_t *ctx, key_t key){
	int i;
	ctx->sock = socket (AF_UNIX, SOCK_STREAM, 0);
	if (!ctx->sock) {
		DBG ("cannot create UNIX socket: %s", strerror(errno));
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < MAX_BIND_ATTEMPTS; i++) {
		struct sockaddr_un addr;
		memset (&addr, 0, sizeof(addr));

		int len;
		addr.sun_family = AF_UNIX;
		ctx->sockid = (key + i) & 0xffff;

		char *socketPath = SUN_PATH_ABSTRACT(&addr);
		{
			char *socketPathSrc = asun_build_path(key);
			asun_path_cpy(socketPath, socketPathSrc);
			free(socketPathSrc);
		}

		len = sizeof(addr);
		if (bind (ctx->sock, (struct sockaddr *)&addr, len) != 0) {
			//DBG ("cannot bind UNIX socket %s: %s, trying next one, len %d", addr.sun_path, strerror(errno), len);
			continue;
		}
		DBG ("bound UNIX socket %s", socketPath);
		break;
	}
	if (i == MAX_BIND_ATTEMPTS) {
		DBG ("cannot bind UNIX socket, bailing out");
		ctx->sockid = -1;
		errno = ENOMEM;
		return -1;
	}
	if (listen (ctx->sock, 4) != 0) {
		DBG ("listen failed");
		errno = ENOMEM;
		return -1;
	}
	pthread_create (&listening_thread_id, NULL, &listening_thread, ctx);
	return 0;
}

int try_get_socket(const char *path){
	struct sockaddr_un addr_stack;
	memset(&addr_stack, 0x00, sizeof(addr_stack));

	struct sockaddr_un *addr = &addr_stack;

	addr->sun_family = AF_UNIX;
	char *socketPath = SUN_PATH_ABSTRACT(addr);
	asun_path_cpy(socketPath, path);
	
	int addrlen = sizeof(*addr);
	DBG ("addr %s", socketPath);

	int fd = socket (AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		DBG ("cannot create UNIX socket: %s", strerror(errno));
		errno = EINVAL;
		return -1;
	}

	if (connect (fd, (struct sockaddr *)addr, addrlen) != 0) {
		DBG ("cannot connect to UNIX socket %s: %s, len %d", addr->sun_path, strerror(errno), addrlen);
		close (fd);
		errno = EINVAL;
		return -1;
	}

	DBG ("connected to socket %s", addr->sun_path);
	return fd;
}

int try_get_remote_fd(key_t key){
	int socket = -1;
	int fd = -1;

	char *socketPath = asun_build_path(key);
	do {
		socket = try_get_socket(socketPath);
		if(socket < 0){
			break;
		}

		if (send(socket, &key, sizeof(key), 0) != sizeof(key)) {
			DBG ("send() failed on socket %s: %s", socketPath, strerror(errno));
			errno = EINVAL;
			break;
		}

		if (ancil_recv_fd (socket, &fd) != 0) {
			DBG ("ERROR: ancil_recv_fd() failed on socket %s: %s", socketPath, strerror(errno));
			errno = EINVAL;
			break;
		}
	} while(0);
	free(socketPath);

	if(socket > -1){
		close(socket);
	}

	return fd;
}

/* Get shared memory segment.  */
int shmget (key_t key, size_t size, int flags) {
	char buf[256];
	int idx;
	shmem_ctx_t *ctx = &gCtx;
	shmem_t *pool = NULL;
	int rc = -1;
	size_t shmid;

	DBG ("key %d size %zu flags 0%o (flags are ignored)", key, size, flags);
	int ashmem_fd = try_get_remote_fd(key);
	if(ashmem_fd < 0){
		if(!listening_thread_id){
			rc = create_listener(ctx, key);
			if(rc != 0){
				return rc;
			}
		}
	} else {
		ctx->sockid = key;
	}

	pthread_mutex_lock (&mutex);
	do {
		idx = ctx->shmem_amount;
		snprintf (buf, sizeof(buf), SOCKNAME "-%d", ctx->sockid, idx);
		DBG("region name: %s", buf);

		ctx->shmem_counter = (ctx->shmem_counter + 1) & 0x7fff;
		shmid = ctx->shmem_counter;
		pool = shmem_resize(ctx, ++ctx->shmem_amount);
		size = ROUND_UP(size, getpagesize ());

		pool[idx].size = size;
		pool[idx].descriptor = (ashmem_fd > -1) ? ashmem_fd : ashmem_create_region (buf, size);
		pool[idx].addr = MAP_FAILED;
		pool[idx].id = get_shmid(ctx, shmid);
		pool[idx].key = key;
		pool[idx].markedForDeletion = 0;
		if (pool[idx].descriptor < 0) {
			DBG ("ashmem_create_region() failed for size %zu: %s", size, strerror(errno));
			pool = shmem_resize(ctx, --ctx->shmem_amount);
			break;
		}
		rc = 0;
		DBG ("ID %d shmid %x FD %d size %zu", idx, get_shmid(ctx, shmid), pool[idx].descriptor, pool[idx].size);
	} while(0);

	pthread_mutex_unlock (&mutex);
	if(rc != 0){
		return rc;
	}

	return get_shmid(ctx, shmid);
}

/* Attach shared memory segment.  */
void *shmat (int shmid, const void *shmaddr, int shmflg) {
	int idx;
	shmem_ctx_t *ctx = &gCtx;
	shmem_t *pool = ctx->pool;

	if(shmaddr == MAP_FAILED){
		errno = -EINVAL;
		return MAP_FAILED;
	}

	DBG ("shmid %x shmaddr %p shmflg %d", shmid, shmaddr, shmflg);
	if (shmaddr != NULL) {
		DBG ("shmaddr != NULL not supported");
		errno = EINVAL;
		return MAP_FAILED;
	}

	shmem_t *mem = NULL;

	pthread_mutex_lock (&mutex);
	do {
		idx = shm_find_by_id (ctx, shmid);
		if (idx == -1){
			DBG ("shmid %x does not exist", shmid);
			errno = EINVAL;
			break;
		}

		mem = &pool[idx];

		if (mem->addr == MAP_FAILED) {
			mem->addr = mmap(
				NULL, mem->size,
				PROT_READ | (shmflg == 0 ? PROT_WRITE : 0),
				MAP_SHARED,
				mem->descriptor, 0
			);
			if (mem->addr == MAP_FAILED) {
				DBG ("mmap() failed for ID %x FD %d: %s", idx, mem->descriptor, strerror(errno));
			}
		}

		DBG ("mapped addr %p for FD %d ID %d", mem->addr, mem->descriptor, idx);
	} while(0);

	pthread_mutex_unlock (&mutex);
	return mem ? mem->addr : MAP_FAILED;
}

static void delete_shmem(shmem_ctx_t *ctx, int idx) {
	shmem_t *pool = ctx->pool;

	if (pool[idx].descriptor){
		close (pool[idx].descriptor);
	}
	ctx->shmem_amount --;
	memmove (
		&pool[idx],
		&pool[idx+1],
		(ctx->shmem_amount - idx) * sizeof(shmem_t)
	);
}

/* Detach shared memory segment.  */
int shmdt (const void *shmaddr) {
	int rc = -1;

	shmem_ctx_t *ctx = &gCtx;
	shmem_t *pool = ctx->pool;

	pthread_mutex_lock (&mutex);
	do {
		int idx = shm_find_by_addr(ctx, shmaddr);
		if(idx < 0){
			DBG ("invalid address %p", shmaddr);
			errno = EINVAL;
			break;
		}

		shmem_t *mem = &pool[idx];
		if (munmap (mem->addr, mem->size) < 0) {
			DBG ("munmap %p failed", shmaddr);
			break;
		}
		mem->addr = MAP_FAILED;
		DBG ("unmapped addr %p for FD %d ID %d shmid %x", shmaddr, mem->descriptor, idx, mem->id);
		if (mem->markedForDeletion || get_sockid(mem->id) != ctx->sockid) {
			DBG ("deleting shmid %x", mem->id);
			delete_shmem(ctx, idx);
		}
		rc = 0;
	} while(0);
	pthread_mutex_unlock (&mutex);

	if(rc != 0){
		DBG ("invalid address %p", shmaddr);
		errno = EINVAL;
	}

	return rc;
}

static int shm_remove (shmem_ctx_t *ctx, int shmid) {
	int idx;
	int rc = 0;
	shmem_t *pool = ctx->pool;

	DBG ("deleting shmid %x", shmid);
	pthread_mutex_lock (&mutex);
	do {
		idx = shm_find_by_id (ctx, shmid);
		if (idx == -1) {
			DBG ("ERROR: shmid %x does not exist", shmid);
			errno = EINVAL;
			rc = -1;
			break;
		}

		shmem_t *mem = &pool[idx];
		if (mem->addr != MAP_FAILED) {
			DBG ("shmid %x is still mapped to addr %p, it will be deleted on shmdt() call", shmid, mem->addr);
			// KDE lib creates shared memory segment, marks it for deletion, and then uses it as if it's not deleted
			mem->markedForDeletion = 1;
			break;
		}
		delete_shmem(ctx, idx);
	} while(0);
	pthread_mutex_unlock (&mutex);

	return rc;
}

static int shm_stat (shmem_ctx_t *ctx, int shmid, struct shmid_ds *buf) {
	int idx;
	int rc = -1;

	shmem_t *pool = ctx->pool;

	pthread_mutex_lock (&mutex);
	do {
		idx = shm_find_by_id (ctx, shmid);
		if (idx == -1) {
			DBG ("ERROR: shmid %x does not exist", shmid);
			errno = EINVAL;
			break;
		}
		if (!buf) {
			DBG ("ERROR: buf == NULL for shmid %x", shmid);
			errno = EINVAL;
			break;
		}

		shmem_t *mem = &pool[idx];

		uid_t uid = geteuid();
		gid_t gid = getegid();

		/* Report max permissive mode */
		memset (buf, 0, sizeof(struct shmid_ds));
		buf->shm_segsz = mem->size;
		buf->shm_nattch = 1;
		buf->shm_perm.__key = mem->key;
		buf->shm_perm.uid = uid;
		buf->shm_perm.gid = gid;
		buf->shm_perm.cuid = uid;
		buf->shm_perm.cgid = gid;
		buf->shm_perm.mode = 0666;
		buf->shm_perm.__seq = 1;
		DBG ("shmid %x size %d", shmid, (int)buf->shm_segsz);

		rc = 0;
	} while(0);
	pthread_mutex_unlock (&mutex);

	return rc;
}

/* Shared memory control operation.  */
int shmctl (int shmid, int cmd, struct shmid_ds *buf) {
	//DBG ("%s: shmid %x cmd %d buf %p", __PRETTY_FUNCTION__, shmid, cmd, buf);

	shmem_ctx_t *ctx = &gCtx;
	switch(cmd){
		case IPC_RMID: return shm_remove (ctx, shmid);
		case IPC_STAT: return shm_stat (ctx, shmid, buf);
	}

	DBG ("cmd %d not implemented yet!", cmd);
	errno = EINVAL;
	return -1;
}
