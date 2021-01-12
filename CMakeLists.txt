cmake_minimum_required(VERSION 3.14)
project(android-shmem)

include(CheckPIESupported)
check_pie_supported()
set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)

add_library(ashmem
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
target_compile_definitions(ashmem PUBLIC
	ASHMEM_STDOUT_LOGGING
)

target_compile_options(ashmem PUBLIC -Wall -Wextra)

target_link_options(ashmem PUBLIC
	LINKER:--version-script=${CMAKE_SOURCE_DIR}/exports.txt
)

add_executable(test test.c)
target_include_directories(test PUBLIC
	${CMAKE_SOURCE_DIR}
)
target_link_libraries(test ashmem log)