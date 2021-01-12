project(android-shmem)

add_library(ashmem SHARED
	ashmem-dev.c
	fd_recv.c
	fd_send.c
	shmem.c
	shmem_socket.c
)
target_include_directories(ashmem PUBLIC
	${CMAKE_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/libancillary
)
target_compile_options(ashmem PUBLIC -Wall -Wextra)

target_link_options(ashmem PUBLIC
	LINKER:--version-script=${CMAKE_SOURCE_DIR}/exports.txt
)