#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>

#define STRINGIFY(x) STRINGIFY2(x)
#define STRINGIFY2(x) #x

#define LOG_PREFIX "[" __FILE__ ":%s:" STRINGIFY(__LINE__) "] "

#ifdef __ANDROID__
#include <android/log.h>
#include "sys/shm.h"


#ifdef NDEBUG
#define DBG(fmt, ...) do {} while (0)
#else
#define DBG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "shmem", LOG_PREFIX fmt, STRINGIFY(__PRETTY_FUNC__), ##__VA_ARGS__)
#endif

#else /* __ANDROID__ */
#include <sys/shm.h>

#define DBG(fmt, ...) fprintf(stderr, LOG_PREFIX fmt "\n", STRINGIFY(__PRETTY_FUNC__), ##__VA_ARGS__)
#endif /* __ANDROID__ */

#define UNUSED(x) (void)(x)

#include "libancillary/ancillary.h"
#include "cutils/ashmem.h"

#include "shmem.h"
#include "shmem_socket.h"

static pthread_t listening_thread_id = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static shmem_ctx_t ctx;

static shmem_t *shmem = NULL;
static size_t shmem_amount = 0;
static size_t shmem_counter = 0;


static int shm_find_id(int shmid){
	unsigned int i;
	for (i = 0; i < shmem_amount; i++) {
		if (shmem[i].id == shmid){
			return i;
		}
	}
	DBG ("cannot find shmid %x", shmid);
	return -1;
}

void *shmem_resize(int shmem_amount){
	shmem = realloc(shmem, shmem_amount * sizeof(shmem_t));
	return shmem;
}

static void *listening_thread(void *arg) {
	UNUSED(arg);

	struct sockaddr_un addr;
	socklen_t len = sizeof(addr);
	int sendsock;
	DBG ("thread started");
	while ((sendsock = accept(ctx.sock, (struct sockaddr *)&addr, &len)) != -1) {
		unsigned int shmid;
		int idx;
		if (recv (sendsock, &idx, sizeof(idx), 0) != sizeof(idx)) {
			DBG ("ERROR: recv() returned not %d bytes", (int)sizeof(idx));
			close (sendsock);
			continue;
		}
		pthread_mutex_lock (&mutex);
		{
			shmid = get_shmid(&ctx, idx);
			idx = shm_find_id (shmid);
			if (idx != -1) {
				if (ancil_send_fd (sendsock, shmem[idx].descriptor) != 0) {
					DBG ("ERROR: ancil_send_fd() failed: %s", strerror(errno));
				}
			} else {
				DBG ("ERROR: cannot find shmid 0x%x", shmid);
			}
		}
		pthread_mutex_unlock (&mutex);
		close (sendsock);
		len = sizeof(addr);
	}
	DBG ("ERROR: listen() failed, thread stopped");
	return NULL;
}

static int create_listener(){
	int i;
	ctx.sock = socket (AF_UNIX, SOCK_STREAM, 0);
	if (!ctx.sock) {
		DBG ("cannot create UNIX socket: %s", strerror(errno));
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < 4096; i++) {
		struct sockaddr_un addr;
		int len;
		memset (&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		ctx.sockid = (getpid() + i) & 0xffff;
		sprintf (&addr.sun_path[1], SOCKNAME, ctx.sockid);
		len = sizeof(addr.sun_family) + strlen(&addr.sun_path[1]) + 1;
		if (bind (ctx.sock, (struct sockaddr *)&addr, len) != 0) {
			//DBG ("%s: cannot bind UNIX socket %s: %s, trying next one, len %d", __PRETTY_FUNCTION__, &addr.sun_path[1], strerror(errno), len);
			continue;
		}
		DBG ("bound UNIX socket %s", addr.sun_path + 1);
		break;
	}
	if (i == 4096) {
		DBG ("cannot bind UNIX socket, bailing out");
		ctx.sockid = 0;
		errno = ENOMEM;
		return -1;
	}
	if (listen (ctx.sock, 4) != 0) {
		DBG ("listen failed");
		errno = ENOMEM;
		return -1;
	}
	pthread_create (&listening_thread_id, NULL, &listening_thread, NULL);
	return 0;
}

/* Get shared memory segment.  */
int shmget (key_t key, size_t size, int flags) {
	char buf[256];
	int idx;

	DBG ("key %d size %zu flags 0%o (flags are ignored)", key, size, flags);
	if (key != IPC_PRIVATE) {
		DBG ("key %d != IPC_PRIVATE,  this is not supported", key);
		errno = EINVAL;
		return -1;
	}
	if (!listening_thread_id) {
		int ret = create_listener();
		if(ret != 0){
			return ret;
		}
	}

	int rc = -1;
	size_t shmid;

	pthread_mutex_lock (&mutex);
	do {
		idx = shmem_amount;
		snprintf (buf, sizeof(buf), SOCKNAME "-%d", ctx.sockid, idx);
		shmem_counter = (shmem_counter + 1) & 0x7fff;
		shmid = shmem_counter;
		shmem_resize(++shmem_amount);
		size = ROUND_UP(size, getpagesize ());
		shmem[idx].size = size;
		shmem[idx].descriptor = ashmem_create_region (buf, size);
		shmem[idx].addr = NULL;
		shmem[idx].id = get_shmid(&ctx, shmid);
		shmem[idx].markedForDeletion = 0;
		if (shmem[idx].descriptor < 0) {
			DBG ("ashmem_create_region() failed for size %zu: %s", size, strerror(errno));
			shmem_resize(--shmem_amount);
			break;
		}
		rc = 0;
		DBG ("ID %d shmid %x FD %d size %zu", idx, get_shmid(&ctx, shmid), shmem[idx].descriptor, shmem[idx].size);
	} while(0);

	pthread_mutex_unlock (&mutex);
	if(rc != 0){
		return rc;
	}

	return get_shmid(&ctx, shmid);
}

int create_client(int shmid, int sid, int *pidx, struct sockaddr_un *addr){
	int addrlen;
	int recvsock;
	DBG ("sockid %x", sid);

	*pidx = get_index(shmid);
	memset (&addr, 0, sizeof(addr));
	addr->sun_family = AF_UNIX;
	sprintf (&addr->sun_path[1], SOCKNAME, sid);
	addrlen = sizeof(addr->sun_family) + strlen(&addr->sun_path[1]) + 1;

	DBG ("addr %s", &addr->sun_path[1]);

	recvsock = socket (AF_UNIX, SOCK_STREAM, 0);
	if (!recvsock) {
		DBG ("cannot create UNIX socket: %s", strerror(errno));
		errno = EINVAL;
		return -1;
	}
	if (connect (recvsock, (struct sockaddr *)addr, addrlen) != 0) {
		DBG ("cannot connect to UNIX socket %s: %s, len %d", addr->sun_path + 1, strerror(errno), addrlen);
		close (recvsock);
		errno = EINVAL;
		return -1;
	}

	DBG ("connected to socket %s", &addr->sun_path[1]);

	return recvsock;
}

int receive_fd(int shmid, int sid, int *pidx){
	int idx;
	struct sockaddr_un addr;
	int descriptor;

	int rc = -1;

	int recvsock = create_client(shmid, sid, &idx, &addr);
	if(recvsock < 0){
		return rc;
	}

	do {
		if (send (recvsock, &idx, sizeof(idx), 0) != sizeof(idx)) {
			DBG ("send() failed on socket %s: %s", addr.sun_path + 1, strerror(errno));
			errno = EINVAL;
			break;
		}

		if (ancil_recv_fd (recvsock, &descriptor) != 0) {
			DBG ("ERROR: ancil_recv_fd() failed on socket %s: %s", addr.sun_path + 1, strerror(errno));
			errno = EINVAL;
			break;
		}

		rc = 0;
	} while(0);
	close (recvsock);

	if(rc < 0){
		return rc;
	}

	*pidx = idx;
	return descriptor;
}

int shmem_new_seg(int shmid, int fd, int size){
	pthread_mutex_lock (&mutex);

	int idx = shmem_amount;
	shmem_resize(++shmem_amount);
	shmem[idx].id = shmid;
	shmem[idx].descriptor = fd;
	shmem[idx].size = size;
	shmem[idx].addr = NULL;
	shmem[idx].markedForDeletion = 0;
	DBG ("created new remote shmem ID %d shmid %x FD %d size %zu", idx, shmid, shmem[idx].descriptor, shmem[idx].size);
	return idx;
}

/* Attach shared memory segment.  */
void *shmat (int shmid, const void *shmaddr, int shmflg) {
	int idx;
	int sid = get_sockid(shmid);
	void *addr;
	DBG ("shmid %x shmaddr %p shmflg %d", shmid, shmaddr, shmflg);

	if (shmaddr != NULL) {
		DBG ("shmaddr != NULL not supported");
		errno = EINVAL;
		return (void *)-1;
	}

	pthread_mutex_lock (&mutex);
	idx = shm_find_id (shmid);

	if (idx == -1){
		if(sid != ctx.sockid){
			int size;
			int descriptor = receive_fd(shmid, sid, &idx);

			DBG ("got FD %d", descriptor);

			size = ashmem_get_size_region(descriptor);
			if (size == 0 || size == -1) {
				DBG ("ERROR: ashmem_get_size_region() returned %d on socket %d: %s", size, sid, strerror(errno));
				errno = EINVAL;
				return (void *)-1;
			}

			DBG ("got size %d", size);
			idx = shmem_new_seg(shmid, descriptor, size);
		}

		if (idx == -1){
			pthread_mutex_unlock (&mutex);
			DBG ("shmid %x does not exist", shmid);
			errno = EINVAL;
			return (void *)-1;
		}
	}

	addr = shmem[idx].addr;
	if (addr == NULL) {
		addr = mmap(
			NULL, shmem[idx].size,
			PROT_READ | (shmflg == 0 ? PROT_WRITE : 0),
			MAP_SHARED,
			shmem[idx].descriptor, 0
		);
		if (addr == MAP_FAILED) {
			DBG ("mmap() failed for ID %x FD %d: %s", idx, shmem[idx].descriptor, strerror(errno));
			addr = NULL;
		}
		shmem[idx].addr = addr;
	}

	DBG ("mapped addr %p for FD %d ID %d", addr, shmem[idx].descriptor, idx);
	pthread_mutex_unlock (&mutex);
	return addr ? addr : (void *)-1;
}

static void delete_shmem(int idx) {
	if (shmem[idx].descriptor){
		close (shmem[idx].descriptor);
	}
	shmem_amount --;
	memmove (
		&shmem[idx],
		&shmem[idx+1],
		(shmem_amount - idx) * sizeof(shmem_t)
	);
}

/* Detach shared memory segment.  */
int shmdt (const void *shmaddr) {
	unsigned int i;
	int rc = -1;

	pthread_mutex_lock (&mutex);
	for (i = 0; i < shmem_amount; i++) {
		if (shmem[i].addr == shmaddr) {
			if (munmap (shmem[i].addr, shmem[i].size) != 0) {
				DBG ("munmap %p failed", shmaddr);
			}
			shmem[i].addr = NULL;
			DBG ("unmapped addr %p for FD %d ID %d shmid %x", shmaddr, shmem[i].descriptor, i, shmem[i].id);
			if (shmem[i].markedForDeletion || get_sockid(shmem[i].id) != ctx.sockid) {
				DBG ("deleting shmid %x", shmem[i].id);
				delete_shmem(i);
			}
			rc = 0;
			break;
		}
	}
	pthread_mutex_unlock (&mutex);

	if(rc != 0){
		DBG ("invalid address %p", shmaddr);
		errno = EINVAL;
	}

	return rc;
}

static int shm_remove (int shmid) {
	int idx;
	int rc = 0;

	DBG ("deleting shmid %x", shmid);
	pthread_mutex_lock (&mutex);
	do {
		idx = shm_find_id (shmid);
		if (idx == -1) {
			DBG ("ERROR: shmid %x does not exist", shmid);
			errno = EINVAL;
			rc = -1;
			break;
		}

		if (shmem[idx].addr) {
			DBG ("shmid %x is still mapped to addr %p, it will be deleted on shmdt() call", shmid, shmem[idx].addr);
			// KDE lib creates shared memory segment, marks it for deletion, and then uses it as if it's not deleted
			shmem[idx].markedForDeletion = 1;
			break;
		}
		delete_shmem(idx);
	} while(0);
	pthread_mutex_unlock (&mutex);

	return rc;
}

static int shm_stat (int shmid, struct shmid_ds *buf) {
	int idx;
	int rc = -1;

	pthread_mutex_lock (&mutex);
	do {
		idx = shm_find_id (shmid);
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

		uid_t uid = geteuid();
		gid_t gid = getegid();

		/* Report max permissive mode */
		memset (buf, 0, sizeof(struct shmid_ds));
		buf->shm_segsz = shmem[idx].size;
		buf->shm_nattch = 1;
		buf->shm_perm.__key = IPC_PRIVATE;
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

	switch(cmd){
		case IPC_RMID: return shm_remove (shmid);
		case IPC_STAT: return shm_stat (shmid, buf);
	}

	DBG ("cmd %d not implemented yet!", cmd);
	errno = EINVAL;
	return -1;
}
