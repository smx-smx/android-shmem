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

add_executable(test_server test_server.c)
target_include_directories(test_server PUBLIC
	${CMAKE_SOURCE_DIR}
)
target_link_libraries(test_server ashmem log)

add_executable(test_client test_client.c)
target_include_directories(test_client PUBLIC
	${CMAKE_SOURCE_DIR}
)
target_link_libraries(test_client ashmem log)

## adb test commands

add_custom_target(adb_copy_server
	COMMAND adb push $<TARGET_FILE:test_server> /data/local/tmp/shm_server
	VERBATIM
)
add_dependencies(adb_copy_server test_server)

add_custom_target(adb_copy_client
	COMMAND adb push $<TARGET_FILE:test_client> /data/local/tmp/shm_client
	VERBATIM
)
add_dependencies(adb_copy_client test_client)

add_custom_target(run_server ALL 
	COMMAND adb shell "/data/local/tmp/shm_server;echo -ne"
	VERBATIM
)
add_dependencies(run_server adb_copy_server)

add_custom_target(run_client ALL
	COMMAND adb shell "/data/local/tmp/shm_client $(pidof shm_server);echo -ne"
	VERBATIM
)
add_dependencies(run_client adb_copy_client)