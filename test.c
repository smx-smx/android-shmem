#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/mman.h>

#define UNUSED(x) (void)(x)

int main(int argc, char *argv[]){
	UNUSED(argc);
	UNUSED(argv);

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	key_t key = IPC_PRIVATE;
	int shm_id = shmget(key, getpagesize(), IPC_CREAT);
	if(shm_id < 0){
		fprintf(stderr, "shmget() failed\n");
		return 1;
	}

	void *mem = shmat(shm_id, NULL, 0);
	printf("mem: %p\n", mem);

	if(mem != MAP_FAILED){
		shmdt(mem);
	}

	if(shmctl(shm_id, IPC_RMID, NULL) < 0){
		fprintf(stderr, "shmctl(SHM_RMID) failed\n");
		return 1;
	}

	return 0;
}