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
#define SERVER "172.30.8.6" // ip address of NFS server
#define EXPORT "/mnt/myshareddir" // exported directory of NFS server
#define NFSFILE "/books/classics/dracula.txt" // path within exported directory to file to read
#define NFSDIR "/books/classics/" // containing directory of NFSFILE
#define BYTES_READ (1024 * 1024 * 512)
#define BYTES_WRITE (1024 * 1024 * 512)

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
	printf("Size %d\n", (int)st->nfs_size);
	printf("Inode %04o\n", (int)st->nfs_ino);

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

	printf("read successful with %d bytes of data\n", status);
	printf("Time of read was: %f seconds\n", time_diff);

	int BYTES_READ_DISPLAY = 16;
	read_data = data;
	printf("Displaying %d bytes of data\n", BYTES_READ_DISPLAY);
	for (i=0; i < BYTES_READ_DISPLAY; i++) {
		printf("%02x ", read_data[i]&0xff);
	}
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
	printf("Read first %d bytes\n", BYTES_READ);

	// begin measurement
	clock_gettime(CLOCK_MONOTONIC, &time_start);
	if (nfs_pread_async(nfs, nfsfh, 0, BYTES_READ, nfs_read_cb, client) != 0) {
		printf("Failed to start async nfs open\n");
		exit(10);
	}
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
	memset(char_buf, 'c', BYTES_WRITE);

	printf("Write first %d bytes\n", BYTES_WRITE);

	// begin measurement
	clock_gettime(CLOCK_MONOTONIC, &time_start);
	if (nfs_pwrite_async(nfs, nfsfh, 0, BYTES_WRITE, char_buf, nfs_write_cb, client) != 0) {
		printf("Failed to start async nfs open\n");
		exit(10);
	}
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
	printf("Size %d\n", (int)st->nfs_size);
	printf("Inode %04o\n", (int)st->nfs_ino);

	printf("Open file for reading :%s\n", NFSFILE);
	if (nfs_open_async(nfs, NFSFILE, O_RDONLY, nfs_open_cb_write, client) != 0) {
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

	printf("Got reply from server for MOUNT/MNT procedure.\n");
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
