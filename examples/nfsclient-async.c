/* 
   Copyright (C) by Ronnie Sahlberg <ronniesahlberg@gmail.com> 2010
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

/* Example program using the highlevel async interface.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#include <win32/win32_compat.h>
#pragma comment(lib, "ws2_32.lib")
WSADATA wsaData;
#else
#include <sys/stat.h>
#endif
 
#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

// Time instrumentation used for profiling the code
#include <time.h>

struct timespec time_start, time_end;
double time_diff;

void set_time_diff(struct timespec* time_start_pointer, struct timespec* time_end_pointer, double* time_diff_pointer) {
	*time_diff_pointer = (time_end_pointer->tv_sec - time_start_pointer->tv_sec) 
		+ 1e-9 * (time_end_pointer->tv_nsec - time_start_pointer->tv_nsec);
}

// Character buffer for writing to NFS File
#include <string.h>
char* char_buf = NULL;


// Input parameters for client; configure these as needed
#define SERVER "172.30.8.223" // ip address of NFS server
#define EXPORT "/mnt/ramdisk" // exported directory of NFS server
#define NFSFILE "/books/classics/dracula.txt" // path within exported directory to file to read
#define NFSDIR "/books/classics/" // containing directory of NFSFILE

// Input parameters for parallel writes in batches done in series
#define WRITE_OFFSET 196
#define NUM_BATCH_WRITES 2
#define NUM_PWRITES 1024
#define BYTES_WRITE (1024 * 1024)
#define WRITE_CHAR 'a'

// Input parameters for parallel reads in batches done in series
#define READ_OFFSET 0
#define NUM_BATCH_READS 1
#define NUM_PREADS 1014
#define BYTES_READ (5 * 1024 * 1024)

char read_buf[BYTES_READ];

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <fcntl.h>
#include "libnfs.h"
#include "libnfs-raw.h"
#include "libnfs-raw-mount.h"

struct rpc_context *mount_context;

struct client {
       char *server;
       char *export;
       uint32_t mount_port;
       struct nfsfh *nfsfh;
       int is_finished;
};

/**
 * Used to keep track of completed writes for pwrite batch async
*/
struct batch_pwrite_cb_data {
	void* private_data;
	nfs_cb batch_cb;
	uint64_t completed_pwrites;
	uint64_t num_pwrites;
};

/**
 * Used to keep track of completed reads for pread batch async
*/
struct batch_pread_cb_data {
	void* private_data;
	nfs_cb batch_cb;
	uint64_t completed_preads;
	uint64_t num_preads;
};

struct batch_series_pread_cb_data {
	void* private_data;
	nfs_cb batch_series_cb;
	struct nfsfh *nfsfh;
	uint64_t completed_batch_reads;
	uint64_t num_batch_reads;
	uint64_t offset;
	uint64_t num_preads;
	uint64_t pread_count;

	// used for timing the entire operation
	struct timespec* start_time;
	struct timespec* end_time;
};


struct batch_series_pwrite_cb_data {
	void* private_data;
	nfs_cb batch_series_cb;
	struct nfsfh *nfsfh;
	uint64_t completed_batch_writes;
	uint64_t num_batch_writes;
	uint64_t offset;
	uint64_t num_pwrites;
	uint64_t pwrite_count;
	const void *buf;

	// used for timing the entire operation
	struct timespec* start_time;
	struct timespec* end_time;
};

/**
 * Callback for a batch read operation within a series; increments the batch
 * counter, checks for termination; if so, calls the batch series callback,
 * otherwise recalls the batch read operation
*/
void nfs_pread_async_batch_series_cb(int status, struct nfs_context *nfs, void* data, void* private_data);

/**
 * Callback for a batch write operation within a series; increments the batch
 * counter, checks for termination; if so, calls the batch series callback,
 * otherwise recalls the batch write operation
*/
void nfs_pwrite_async_batch_series_cb(int status, struct nfs_context *nfs, void* data, void* private_data);

/**
 * The callback for a single pread within a batch operation.  Increments the total count of 
 * completed reads within the batch and launches the batch callback if all
 * reads are complete
*/
void nfs_batch_single_pread_cb(int status, struct nfs_context *nfs, void *data, void *private_data);

/**
 * The callback for a single pwrite within a batch operation.  Increments the total count of 
 * completed writes within the batch and launches the batch callback if all
 * writes are complete
*/
void nfs_batch_single_pwrite_cb(int status, struct nfs_context *nfs, void *data, void *private_data);

/**
 * Allocate the pointer and add relevant data structures
*/
int initialize_batch_pread_cb_data_ptr(struct batch_pread_cb_data **data_ptr, 
	struct nfs_context *nfs, void *private_data, uint64_t num_preads, nfs_cb cb)
{
	*data_ptr = (struct batch_pread_cb_data*) malloc(sizeof(**data_ptr));
	if (*data_ptr == NULL) {
			nfs_set_error(nfs, "Out of memory. Failed to allocate "
							"cb data");
			return -1;
	}
	memset(*data_ptr, 0, sizeof(**data_ptr));

	(*data_ptr)->private_data = private_data;
	(*data_ptr)->completed_preads = 0;
	(*data_ptr)->num_preads = num_preads;
	(*data_ptr)->batch_cb = cb;
	return 0;
}

/**
 * Allocate the pointer and add relevant data structures
*/
int initialize_batch_pwrite_cb_data_ptr(struct batch_pwrite_cb_data **data_ptr, 
	struct nfs_context *nfs, void *private_data, uint64_t num_pwrites, nfs_cb cb)
{
	*data_ptr = (struct batch_pwrite_cb_data*) malloc(sizeof(**data_ptr));
	if (*data_ptr == NULL) {
			nfs_set_error(nfs, "Out of memory. Failed to allocate "
							"cb data");
			return -1;
	}
	memset(*data_ptr, 0, sizeof(**data_ptr));

	(*data_ptr)->private_data = private_data;
	(*data_ptr)->completed_pwrites = 0;
	(*data_ptr)->num_pwrites = num_pwrites;
	(*data_ptr)->batch_cb = cb;
	return 0;
}

int initialize_batch_series_pread_cb_data_ptr(struct batch_series_pread_cb_data** data_ptr, 
			struct nfs_context *nfs, struct nfsfh *nfsfh, uint64_t offset,
            uint64_t num_batch_reads, uint64_t num_preads, uint64_t pread_count,
			nfs_cb cb, void *private_data) 
{
	*data_ptr = (struct batch_series_pread_cb_data*) malloc(sizeof(**data_ptr));
	if (*data_ptr == NULL) {
			nfs_set_error(nfs, "Out of memory. Failed to allocate "
							"cb data");
			return -1;
	}
	memset(*data_ptr, 0, sizeof(**data_ptr));

	(*data_ptr)->private_data = private_data;
	(*data_ptr)->batch_series_cb = cb;
	(*data_ptr)->completed_batch_reads = 0;
	(*data_ptr)->num_batch_reads = num_batch_reads;
	(*data_ptr)->num_preads = num_preads;
	(*data_ptr)->nfsfh = nfsfh;
	(*data_ptr)->offset = offset;
	(*data_ptr)->pread_count = pread_count;
	(*data_ptr)->start_time = (struct timespec *) malloc(sizeof(struct timespec));
	(*data_ptr)->end_time = (struct timespec *) malloc(sizeof(struct timespec));

	clock_gettime(CLOCK_MONOTONIC, (*data_ptr)->start_time);

    return 0;
}

/**
 * Allocate the pointer and add relevant data structures
*/
int initialize_batch_series_pwrite_cb_data_ptr(struct batch_series_pwrite_cb_data** data_ptr, 
			struct nfs_context *nfs, struct nfsfh *nfsfh, uint64_t offset,
            uint64_t num_batch_writes, uint64_t num_pwrites, uint64_t pwrite_count,
			const void *buf, nfs_cb cb, void *private_data) 
{
	*data_ptr = (struct batch_series_pwrite_cb_data*) malloc(sizeof(**data_ptr));
	if (*data_ptr == NULL) {
			nfs_set_error(nfs, "Out of memory. Failed to allocate "
							"cb data");
			return -1;
	}
	memset(*data_ptr, 0, sizeof(**data_ptr));

	(*data_ptr)->private_data = private_data;
	(*data_ptr)->batch_series_cb = cb;
	(*data_ptr)->completed_batch_writes = 0;
	(*data_ptr)->num_batch_writes = num_batch_writes;
	(*data_ptr)->num_pwrites = num_pwrites;
	(*data_ptr)->nfsfh = nfsfh;
	(*data_ptr)->offset = offset;
	(*data_ptr)->pwrite_count = pwrite_count;
	(*data_ptr)->buf = buf;
	(*data_ptr)->start_time = (struct timespec *) malloc(sizeof(struct timespec));
	(*data_ptr)->end_time = (struct timespec *) malloc(sizeof(struct timespec));

	clock_gettime(CLOCK_MONOTONIC, (*data_ptr)->start_time);

    return 0;
}


void nfs_pread_series_async(struct nfs_context *nfs, struct nfsfh *nfsfh, uint64_t offset,
                uint64_t num_batch_reads, uint64_t num_preads, uint64_t pread_count, 
				nfs_cb cb, void *private_data) {
	struct batch_series_pread_cb_data* data;
	initialize_batch_series_pread_cb_data_ptr(&data, nfs, nfsfh, offset,
        num_batch_reads, num_preads, pread_count, cb, private_data);

	nfs_pread_batch_async(nfs, nfsfh, offset, num_preads, pread_count, 
		nfs_pread_async_batch_series_cb, data);

}


void nfs_pwrite_series_async(struct nfs_context *nfs, struct nfsfh *nfsfh, uint64_t offset,
			uint64_t num_batch_writes, uint64_t num_pwrites, uint64_t pwrite_count,
			const void *buf, nfs_cb cb, void *private_data) {
	struct batch_series_pwrite_cb_data* data;
	initialize_batch_series_pwrite_cb_data_ptr(&data, nfs, nfsfh, offset,
        num_batch_writes, num_pwrites, pwrite_count, buf, cb, private_data);

	nfs_pwrite_batch_async(nfs, nfsfh, offset, num_pwrites, pwrite_count, 
		buf, nfs_pwrite_async_batch_series_cb, data);
}


/**
 * Writes num_pwrites * pwrite_count bytes to the NFS server in parallel
 * starting at designated offset
*/
void nfs_pwrite_batch_async(struct nfs_context *nfs, struct nfsfh *nfsfh, uint64_t offset,
                 uint64_t num_pwrites, uint64_t pwrite_count, const void *buf, nfs_cb cb, void *private_data) 
{
	struct batch_pwrite_cb_data *data;
	initialize_batch_pwrite_cb_data_ptr(&data, nfs, private_data, num_pwrites, cb);

	for (uint64_t i = 0; i < num_pwrites; i++) {
		if (nfs_pwrite_async(nfs, nfsfh, offset + i * pwrite_count, pwrite_count, buf, nfs_batch_single_pwrite_cb, data) != 0) {
			printf("Failed to start async nfs write\n");
			exit(10);
		}
	}
}

void nfs_pread_batch_async(struct nfs_context *nfs, struct nfsfh *nfsfh, uint64_t offset,
                uint64_t num_preads, uint64_t pread_count, nfs_cb cb, void *private_data)
{
	struct batch_pread_cb_data *data;
	initialize_batch_pread_cb_data_ptr(&data, nfs, private_data, num_preads, cb);

	for (uint64_t i = 0; i < num_preads; i++) {
		if (nfs_pread_async(nfs, nfsfh, offset + i * pread_count, pread_count, nfs_batch_single_pread_cb, data) != 0) {
			printf("Failed to start async nfs write\n");
			exit(10);
		}
	}
}

void mount_export_cb(struct rpc_context *mount_context, int status, void *data, void *private_data)
{
	struct client *client = private_data;
	exports export = *(exports *)data;

	if (status < 0) {
		printf("MOUNT/EXPORT failed with \"%s\"\n", rpc_get_error(mount_context));
		exit(10);
	}

	printf("Got exports list from server %s\n", client->server);
	while (export != NULL) {
	      printf("Export: %s\n", export->ex_dir);
	      export = export->ex_next;
	}

	mount_context = NULL;

	client->is_finished = 1;
}

void nfs_opendir_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct client *client = private_data;
	struct nfsdir *nfsdir = data;
	struct nfsdirent *nfsdirent;

	if (status < 0) {
		printf("opendir failed with \"%s\"\n", (char *)data);
		exit(10);
	}

	printf("opendir successful\n");
	while((nfsdirent = nfs_readdir(nfs, nfsdir)) != NULL) {
		printf("Inode:%d Name:%s\n", (int)nfsdirent->inode, nfsdirent->name);
	}
	nfs_closedir(nfs, nfsdir);

	printf("\nCalling getexports:\n");
	mount_context = rpc_init_context();
	if (mount_getexports_async(mount_context, client->server, mount_export_cb, client) != 0) {
		printf("Failed to start MOUNT/EXPORT\n");
		exit(10);
	}
}

void nfs_close_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct client *client = private_data;

	if (status < 0) {
		printf("close failed with \"%s\"\n", (char *)data);
		exit(10);
	}

	printf("close successful\n");
	printf("\n");
	printf("call opendir(%s)\n", NFSDIR);
	if (nfs_opendir_async(nfs, NFSDIR, nfs_opendir_cb, client) != 0) {
		printf("Failed to start async nfs close\n");
		exit(10);
	}
}

void nfs_fstat64_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct client *client = private_data;
	struct nfs_stat_64 *st;
 
	if (status < 0) {
		printf("fstat call failed with \"%s\"\n", (char *)data);
		exit(10);
	}

	printf("Got reply from server for fstat(%s).\n", NFSFILE);
	st = (struct nfs_stat_64 *)data;
	printf("Mode %04o\n", (int)st->nfs_mode);
	printf("Size %ld\n", st->nfs_size);
	printf("Inode %04o\n", (int)st->nfs_ino);
	printf("\n");

	printf("Close file\n");
	if (nfs_close_async(nfs, client->nfsfh, nfs_close_cb, client) != 0) {
		printf("Failed to start async nfs close\n");
		exit(10);
	}
}

void nfs_read_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	// get callback for completion of function
	clock_gettime(CLOCK_MONOTONIC, &time_end);
	set_time_diff(&time_start, &time_end, &time_diff);

	struct client *client = private_data;
	char *read_data;
	int i;

	if (status < 0) {
		printf("read failed with \"%s\"\n", (char *)data);
		exit(10);
	}

	printf("read successful with %ld bytes of data\n", status);
	printf("Time of read was: %f seconds\n", time_diff);

	int BYTES_READ_DISPLAY = 16;
	read_data = data;
	printf("Displaying %d bytes of data\n", BYTES_READ_DISPLAY);
	for (i=0; i < BYTES_READ_DISPLAY; i++) {
		printf("%02x ", read_data[i]&0xff);
	}
	printf("\n");

	for (i=0; i < BYTES_READ_DISPLAY; i++) {
		printf("%02x ", read_buf[i]&0xff);
	}

	printf(&read_data, " ", &read_buf, "\n");

	printf("\n");
	printf("Fstat file :%s\n", NFSFILE);
	if (nfs_fstat64_async(nfs, client->nfsfh, nfs_fstat64_cb, client) != 0) {
		printf("Failed to start async nfs fstat\n");
		exit(10);
	}
}

void nfs_write_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	// get callback for completion of function
	clock_gettime(CLOCK_MONOTONIC, &time_end);
	set_time_diff(&time_start, &time_end, &time_diff);

	// free char_buf
	free(char_buf);

	struct client *client = private_data;
	char *read_data;
	int i;

	if (status < 0) {
		printf("read failed with \"%s\"\n", (char *)data);
		exit(10);
	}

	printf("write successful with %d bytes of data\n", status);
	printf("Time of write was: %f seconds\n", time_diff);

	printf("\n");
	printf("Fstat file :%s\n", NFSFILE);
	if (nfs_fstat64_async(nfs, client->nfsfh, nfs_fstat64_cb, client) != 0) {
		printf("Failed to start async nfs fstat\n");
		exit(10);
	}
}

void nfs_pread_async_batch_series_cb(int status, struct nfs_context *nfs, void* data, void* private_data) {
	struct batch_series_pread_cb_data *cb_data = private_data;
	cb_data->completed_batch_reads += 1;

	if (status < 0) {
		printf("Batch write failed: %d\n", status);
		exit(10);
	}

	uint64_t bytes_read = cb_data->num_preads * cb_data->pread_count;

	// printf("writing batch %ld successful with %ld bytes of data\n\n", cb_data->completed_batch_writes, bytes_written);

	// get the counter value; comapare to the total needed to complete
	if (cb_data->completed_batch_reads == cb_data->num_batch_reads) {
		// We have completed the operation so we get the time
		clock_gettime(CLOCK_MONOTONIC, cb_data->end_time);
		double time_diff;
		set_time_diff(cb_data->start_time, cb_data->end_time, &time_diff);
		printf("Time of reads was: %f seconds\n", time_diff);
		uint64_t total_bytes_read = cb_data->completed_batch_reads * bytes_read;
		printf("Total written bytes: %ld\n", total_bytes_read);
		printf("Bandwidth: %f MB/s\n", total_bytes_read/ time_diff / 1000000);

		cb_data->batch_series_cb(0, nfs, data, cb_data->private_data);
	}

	else {
		uint64_t current_offset = cb_data->offset + 
			cb_data->completed_batch_reads * bytes_read;

		nfs_pread_batch_async(nfs, cb_data->nfsfh, current_offset, 
		cb_data->num_preads, cb_data->pread_count, 
		nfs_pread_async_batch_series_cb, private_data);
	}
}

void nfs_pwrite_async_batch_series_cb(int status, struct nfs_context *nfs, void* data, void* private_data) {
	struct batch_series_pwrite_cb_data *cb_data = private_data;
	cb_data->completed_batch_writes += 1;

	if (status < 0) {
		printf("Batch write failed: %d\n", status);
		exit(10);
	}

	uint64_t bytes_written = cb_data->num_pwrites * cb_data->pwrite_count;

	// printf("writing batch %ld successful with %ld bytes of data\n\n", cb_data->completed_batch_writes, bytes_written);

	// get the counter value; comapare to the total needed to complete
	if (cb_data->completed_batch_writes == cb_data->num_batch_writes) {
		// We have completed the operation so we get the time
		clock_gettime(CLOCK_MONOTONIC, cb_data->end_time);
		double time_diff;
		set_time_diff(cb_data->start_time, cb_data->end_time, &time_diff);
		printf("Time of writes was: %f seconds\n", time_diff);
		uint64_t total_bytes_written = cb_data->completed_batch_writes * bytes_written;
		printf("Total written bytes: %ld\n", total_bytes_written);
		printf("Bandwidth: %f MB/s\n", total_bytes_written / time_diff / 1000000);

		cb_data->batch_series_cb(0, nfs, data, cb_data->private_data);
	}
	else {
		uint64_t current_offset = cb_data->offset + 
			cb_data->completed_batch_writes * bytes_written;

		nfs_pwrite_batch_async(nfs, cb_data->nfsfh, current_offset, 
		cb_data->num_pwrites, cb_data->pwrite_count, 
		cb_data->buf, nfs_pwrite_async_batch_series_cb, private_data);
	}
}

void nfs_pread_batch_async_cb(int status, struct nfs_context *nfs, void* data, void* private_data) {
	struct client *client = private_data;

	printf("\n");
	printf("Fstat file :%s\n", NFSFILE);
	if (nfs_fstat64_async(nfs, client->nfsfh, nfs_fstat64_cb, client) != 0) {
		printf("Failed to start async nfs fstat\n");
		exit(10);
	}
}

void nfs_pwrite_batch_async_cb(int status, struct nfs_context *nfs, void* data, void* private_data) {
	struct client *client = private_data;

	printf("\n");
	printf("Fstat file :%s\n", NFSFILE);
	if (nfs_fstat64_async(nfs, client->nfsfh, nfs_fstat64_cb, client) != 0) {
		printf("Failed to start async nfs fstat\n");
		exit(10);
	}
}

void nfs_batch_single_pread_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct batch_pread_cb_data *cb_data = private_data;
	cb_data->completed_preads += 1;

	if (status < 0) {
		printf("batch pwrite failed\n");
		exit(10);
	}

	// printf("Read %ld of batch successful\n", cb_data->completed_preads);
	// int BYTES_READ_DISPLAY = 5;
	// char* read_data = (char*) data;
	// printf("Displaying %d bytes of data\n", BYTES_READ_DISPLAY);
	// for (int i=0; i < BYTES_READ_DISPLAY; i++) {
	// 	printf("%02x ", read_data[i]&0xff);
	// 	// char c = read_data[i];
	// 	// printf(c);
	// }

	if (cb_data->completed_preads == cb_data->num_preads) {
		cb_data->batch_cb(0, nfs, data, cb_data->private_data);
	}
}

void nfs_batch_single_pwrite_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct batch_pwrite_cb_data *cb_data = private_data;
	cb_data->completed_pwrites += 1;

	if (status < 0) {
		printf("batch pwrite failed\n");
		exit(10);
	}

	// printf("write %ld of batch successful with %d bytes of data\n", cb_data->completed_pwrites, status);

	if (cb_data->completed_pwrites == cb_data->num_pwrites) {
		cb_data->batch_cb(0, nfs, data, cb_data->private_data);
		// nfs_pwrite_batch_async_cb(nfs, cb_data->private_data);
	}
}

void nfs_open_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct client *client = private_data;
	struct nfsfh *nfsfh;

	if (status < 0) {
		printf("open call failed with \"%s\"\n", (char *)data);
		exit(10);
	}

	nfsfh         = data;
	client->nfsfh = nfsfh;
	printf("Got reply from server for open(%s). Handle:%p\n", NFSFILE, data);
	printf("\nReading first %d bytes\n", BYTES_READ);

	// begin measurement
	clock_gettime(CLOCK_MONOTONIC, &time_start);

	if (nfs3_pread_async_internal_buffer(nfs, nfsfh, 0, BYTES_READ, nfs_read_cb, client, 0, read_buf) != 0) {
		printf("Failed to start async nfs open\n");
		exit(10);
	}
	// if (nfs_pread_async(nfs, nfsfh, 0, BYTES_READ, nfs_read_cb, client) != 0) {
	// 	printf("Failed to start async nfs open\n");
	// 	exit(10);
	// }
	// nfs_pread_batch_async(nfs, nfsfh, 0, 5, BYTES_READ, nfs_pread_batch_async_cb, client);
	// nfs_pread_series_async(nfs, nfsfh, READ_OFFSET, NUM_BATCH_READS, NUM_PREADS, BYTES_READ, nfs_pread_batch_async_cb, client);
}



void nfs_open_cb_write(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct client *client = private_data;
	struct nfsfh *nfsfh;

	if (status < 0) {
		printf("open call failed with \"%s\"\n", (char *)data);
		exit(10);
	}

	nfsfh         = data;
	client->nfsfh = nfsfh;
	printf("Got reply from server for open(%s). Handle:%p\n", NFSFILE, data);

	// Allocate and initialize character buffer
	char_buf = (char *)malloc(BYTES_WRITE);
	assert(char_buf != NULL);
	memset(char_buf, WRITE_CHAR, BYTES_WRITE);

	printf("\nWriting first %d bytes\n", BYTES_WRITE);

	// begin measurement
	clock_gettime(CLOCK_MONOTONIC, &time_start);
	// if (nfs_pwrite_async(nfs, nfsfh, 0, BYTES_WRITE, char_buf, nfs_write_cb, client) != 0) {
	// 	printf("Failed to start async nfs open\n");
	// 	exit(10);
	// }
	nfs_pwrite_series_async(nfs, nfsfh, WRITE_OFFSET, NUM_BATCH_WRITES, NUM_PWRITES, BYTES_WRITE, char_buf, nfs_pwrite_batch_async_cb, client);
}

void nfs_stat64_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct client *client = private_data;
	struct nfs_stat_64 *st;
 
	if (status < 0) {
		printf("stat call failed with \"%s\"\n", (char *)data);
		exit(10);
	}

	printf("Got reply from server for stat(%s).\n", NFSFILE);
	st = (struct nfs_stat_64 *)data;
	printf("Mode %04o\n", (unsigned int) st->nfs_mode);
	printf("Size %ld\n", st->nfs_size);
	printf("Inode %04o\n", (int)st->nfs_ino);
	printf("\n");

	printf("Open file for reading :%s\n", NFSFILE);
	if (nfs_open_async(nfs, NFSFILE, O_RDONLY, nfs_open_cb, client) != 0) {
		printf("Failed to start async nfs open\n");
		exit(10);
	}
}

void nfs_mount_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct client *client = private_data;

	if (status < 0) {
		printf("mount/mnt call failed with \"%s\"\n", (char *)data);
		exit(10);
	}

	printf("Got reply from server for MOUNT/MNT procedure.\n\n");
	printf("Stat file :%s\n", NFSFILE);
	if (nfs_stat64_async(nfs, NFSFILE, nfs_stat64_cb, client) != 0) {
		printf("Failed to start async nfs stat\n");
		exit(10);
	}
}



int main(int argc _U_, char *argv[] _U_)
{
	struct nfs_context *nfs;
	int ret;
	struct client client;
	struct pollfd pfds[2]; /* nfs:0  mount:1 */

#ifdef WIN32
	if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
		printf("Failed to start Winsock2\n");
		exit(10);
	}
#endif

	client.server = SERVER;
	client.export = EXPORT;
	client.is_finished = 0;

	nfs = nfs_init_context();
	if (nfs == NULL) {
		printf("failed to init context\n");
		exit(10);
	}

	ret = nfs_mount_async(nfs, client.server, client.export, nfs_mount_cb, &client);
	if (ret != 0) {
		printf("Failed to start async nfs mount\n");
		exit(10);
	}

	for (;;) {
		int num_fds;

		pfds[0].fd = nfs_get_fd(nfs);
		pfds[0].events = nfs_which_events(nfs);
		num_fds = 1;

		if (mount_context != 0 && rpc_get_fd(mount_context) != -1) {
			pfds[1].fd = rpc_get_fd(mount_context);
			pfds[1].events = rpc_which_events(mount_context);
			num_fds = 2;
		}
		if (poll(&pfds[0], 2, -1) < 0) {
			printf("Poll failed");
			exit(10);
		}
		if (mount_context != NULL) {
			if (rpc_service(mount_context, pfds[1].revents) < 0) {
				printf("rpc_service failed\n");
				break;
			}
		}
		if (nfs_service(nfs, pfds[0].revents) < 0) {
			printf("nfs_service failed\n");
			break;
		}
		if (client.is_finished) {
			break;
		}
	}
	
	nfs_destroy_context(nfs);
	if (mount_context != NULL) {
		rpc_destroy_context(mount_context);
		mount_context = NULL;
	}
	printf("nfsclient finished\n");
	return 0;
}
