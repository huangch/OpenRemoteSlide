/*
 *  OpenRemoteSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2019 huangch
 *
 *  All rights reserved.
 *
 *  OpenRemoteSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "openslide-urlio.h"

#include <glib.h>

static URLIO_CACHE **g_cache_list = NULL;
static int g_cache_count = 0;

static URLIO_CONN **g_urlio_list = NULL;
static int g_urlio_count = 0;

static GMutex g_cache_lock;

void urlio_initial(void) {
#ifdef URLIO_VERBOSE
	printf("initial\n");
#endif

	g_thread_init(NULL);
	curl_global_init(CURL_GLOBAL_ALL);

	return;
}

void urlio_release(void) {
#ifdef URLIO_VERBOSE
	printf("release\n");
#endif

	curl_global_cleanup();

	return;
}

int urlio_ferror(URLIO_FILE *file) {
	int ret = 0;

	switch (file->type) {
	case CFTYPE_FILE:
		ret = ferror(file->handle.file);
		break;

	case CFTYPE_CURL:
		for (int t = 0; t < THREAD_NUM; t++) {
			if ((file->handle.conn->io[t].buffer_pos == 0)
					&& (!file->handle.conn->still_running)) {
				ret = 1;
				break;
			}
		}
		break;
	default: /* unknown or supported type - oh dear */
		ret = -1;
		errno = EBADF;
		break;
	}
	return ret;
}

/* curl calls this routine to get more data */
static size_t write_callback(char *buffer, size_t size, size_t nitems,
		void *userp) {
	char *newbuff;
	size_t rembuff;

	URLIO_IO *io = (URLIO_IO *) userp;
	size *= nitems;

	rembuff = io->buffer_len - io->buffer_pos; /* remaining space in buffer */

	if (size > rembuff) {
		/* not enough space in buffer */
		newbuff = realloc(io->buffer, io->buffer_len + (size - rembuff));
		if (newbuff == NULL) {
			fprintf(stderr, "callback buffer grow failed\n");
			size = rembuff;
		} else {
			/* realloc succeeded increase buffer size*/
			io->buffer_len += size - rembuff;
			io->buffer = newbuff;
		}
	}

	memcpy(&io->buffer[io->buffer_pos], buffer, size);
	io->buffer_pos += size;

	return size;
}

/* use to attempt to fill the read buffer up to requested number of bytes */
static int fill_buffer(URLIO_CONN *conn, size_t want, int thread_index) {
	fd_set fdread;
	fd_set fdwrite;
	fd_set fdexcep;
	struct timeval timeout;
	int rc;
	CURLMcode mc; /* curl_multi_fdset() return code */

	/* only attempt to fill buffer if transactions still running and buffer
	 * doesn't exceed required size already
	 */
	if ((!conn->still_running) || (conn->io[thread_index].buffer_pos > want))
		return 0;

	/* attempt to fill buffer */
	do {
		int maxfd = -1;
		long curl_timeo = -1;

		FD_ZERO(&fdread);
		FD_ZERO(&fdwrite);
		FD_ZERO(&fdexcep);

		/* set a suitable timeout to fail on */
		timeout.tv_sec = 60; /* 1 minute */
		timeout.tv_usec = 0;

		curl_multi_timeout(conn->multi_handle, &curl_timeo);
		if (curl_timeo >= 0) {
			timeout.tv_sec = curl_timeo / 1000;
			if (timeout.tv_sec > 1)
				timeout.tv_sec = 1;
			else
				timeout.tv_usec = (curl_timeo % 1000) * 1000;
		}

		/* get file descriptors from the transfers */
		mc = curl_multi_fdset(conn->multi_handle, &fdread, &fdwrite, &fdexcep,
				&maxfd);

		if (mc != CURLM_OK) {
			fprintf(stderr, "curl_multi_fdset() failed, code %d.\n", mc);
			break;
		}

		/* On success the value of maxfd is guaranteed to be >= -1. We call
		 select(maxfd + 1, ...); specially in case of (maxfd == -1) there are
		 no fds ready yet so we call select(0, ...) --or Sleep() on Windows--
		 to sleep 100ms, which is the minimum suggested value in the
		 curl_multi_fdset() doc. */

		if (maxfd == -1) {
#ifdef _WIN32
			Sleep(100);
			rc = 0;
#else
			/* Portable sleep for platforms other than Windows. */
			struct timeval wait = { 0, 100 * 1000 }; /* 100ms */
			rc = select(0, NULL, NULL, NULL, &wait);
#endif
		} else {
			/* Note that on some platforms 'timeout' may be modified by select().
			 If you need access to the original value save a copy beforehand. */
			rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);
		}

		switch (rc) {
		case -1:
			/* select error */
			break;

		case 0:
		default:
			/* timeout or readable/writable sockets */
			curl_multi_perform(conn->multi_handle, &conn->still_running);
			break;
		}
	} while (conn->still_running && (conn->io[thread_index].buffer_pos < want));
	return 1;
}

/* use to remove want bytes from the front of a files buffer */
static int use_buffer(URLIO_CONN *conn, size_t want, int thread_index) {
	/* sort out buffer */
	if ((conn->io[thread_index].buffer_pos - want) <= 0) {
		/* ditch buffer - write will recreate */
		free(conn->io[thread_index].buffer);
		conn->io[thread_index].buffer = NULL;
		conn->io[thread_index].buffer_pos = 0;
		conn->io[thread_index].buffer_len = 0;
	} else {
		/* move rest down make it available for later */
		memmove(conn->io[thread_index].buffer,
				&conn->io[thread_index].buffer[want],
				(conn->io[thread_index].buffer_pos - want));

		conn->io[thread_index].buffer_pos -= want;
	}
	return 0;
}

static size_t download(void *ptr, size_t pos, size_t wanted, URLIO_CONN *conn) {
	int wanted_bulk_count = (((pos % (CACHE_BULK_SIZE)) + wanted - 1)
			/ (CACHE_BULK_SIZE)) + 1;
	size_t wanted_bulk_id = (pos / (CACHE_BULK_SIZE)) * (CACHE_BULK_SIZE);
	size_t residual = wanted;
	size_t copied = 0L;
	size_t want_per_thread[THREAD_NUM];
	size_t want_total;

	for (int i = 0; i < wanted_bulk_count; i++) {
		int wanted_cache_index = -1;
		int wanted_bulk_index = -1;

		for (int j = 0; j < g_cache_count; j++) {
			if (!strcmp(g_cache_list[j]->url, conn->url)) {
				wanted_cache_index = j;
				break;
			}
		}

		if (wanted_cache_index != -1) {
			for (int j = 0; j < g_cache_list[wanted_cache_index]->bulk_count;
					j++) {
				if (g_cache_list[wanted_cache_index]->bulk_id_list[j]
						== wanted_bulk_id) {
					printf("download: cache hit bulk id %zu\n", wanted_bulk_id);
					wanted_bulk_index = j;
					break;
				}
			}
		}

		if (wanted_bulk_index == -1) { /* missed bulk */
			printf("download: cache is missed\n");

			g_mutex_lock(&g_cache_lock);

			for (int t = 0; t < THREAD_NUM; t++) {
				if (pos + t * (CACHE_BULK_SIZE_PER_THREAD) > conn->size)
					break;
				/* close the stream */
				/* make sure the easy handle is not in the multi handle anymore */
				curl_multi_remove_handle(conn->multi_handle, conn->io[t].curl);

				/* cleanup */
				curl_easy_cleanup(conn->io[t].curl);

				/* reopen the stream and resume at a specific pos */
				conn->io[t].curl = curl_easy_init();

				curl_easy_setopt(conn->io[t].curl, CURLOPT_URL, conn->url);
				curl_easy_setopt(conn->io[t].curl, CURLOPT_WRITEDATA,
						&(conn->io[t]));
				curl_easy_setopt(conn->io[t].curl, CURLOPT_VERBOSE,
						CURL_VERBOSE);
				curl_easy_setopt(conn->io[t].curl, CURLOPT_WRITEFUNCTION,
						write_callback);
				curl_easy_setopt(conn->io[t].curl, CURLOPT_RESUME_FROM_LARGE,
						wanted_bulk_id+t*(CACHE_BULK_SIZE_PER_THREAD)); /* bulk id represents the pointer of the head of a bulk*/

//			/* This tests if multi_handle has been initialzed. It should always be initialized.*/
//			if (!urlio->multi_handle)
//				urlio->multi_handle = curl_multi_init();

				curl_multi_add_handle(conn->multi_handle, conn->io[t].curl);
			}
			/* lets start the fetch */
			curl_multi_perform(conn->multi_handle, &conn->still_running);

			for (int t = 0; t < THREAD_NUM; t++) {
				if (pos + t * (CACHE_BULK_SIZE_PER_THREAD) > conn->size)
					break;
				if ((conn->io[t].buffer_pos == 0) && (!conn->still_running)) {
					/* if still_running is 0 now, we should return NULL */

					for (int q = 0; q < THREAD_NUM; q++) {
						if (pos + q * (CACHE_BULK_SIZE_PER_THREAD) > conn->size)
							break;

						/* make sure the easy handle is not in the multi handle anymore */
						curl_multi_remove_handle(conn->multi_handle,
								conn->io[q].curl);

						/* cleanup */
						curl_easy_cleanup(conn->io[q].curl);
					}

					g_mutex_unlock(&g_cache_lock);
					return 0L;
				}
			}

			/* check if there's data in the buffer - if not fill either error or
			 * EOF */
			for (int t = 0; t < THREAD_NUM; t++) {
				if (pos + t * (CACHE_BULK_SIZE_PER_THREAD) > conn->size)
					break;

				if (conn->io[t].buffer_pos) {
					/* halt transaction */
					curl_multi_remove_handle(conn->multi_handle,
							conn->io[t].curl);

					/* restart */
					curl_multi_add_handle(conn->multi_handle, conn->io[t].curl);

					/* ditch buffer - write will recreate - resets stream pos*/
					free(conn->io[t].buffer);
					conn->io[t].buffer = NULL;
					conn->io[t].buffer_pos = 0L;
					conn->io[t].buffer_len = 0L;
				}
			}

			/* This paragraph reads want of data from the stream */

			for (int t = 0; t < THREAD_NUM; t++) {
				if (pos + t * (CACHE_BULK_SIZE_PER_THREAD) > conn->size)
					break;

				want_per_thread[t] = (CACHE_BULK_SIZE_PER_THREAD);

				fill_buffer(conn, want_per_thread[t], t);

				/* check if there's data in the buffer - if not fill_buffer()
				 * either error or EOF */
				if (!conn->io[t].buffer_pos) {
					g_mutex_unlock(&g_cache_lock);
					return 0L;
				}

				/* ensure only available data is considered */
				if (conn->io[t].buffer_pos < want_per_thread[t])
					want_per_thread[t] = conn->io[t].buffer_pos;

			}

			want_total = 0L;
			for (int t = 0; t < THREAD_NUM; t++) {
				if (pos + t * (CACHE_BULK_SIZE_PER_THREAD) > conn->size)
					break;

				want_total += want_per_thread[t];
			}

			/* Expand cache memory */
			if (wanted_cache_index == -1) {
				if (!g_cache_list)
					g_cache_list = (URLIO_CACHE**) malloc(sizeof(URLIO_CACHE*));
				else
					g_cache_list = (URLIO_CACHE**) realloc(g_cache_list,
							(g_cache_count + 1) * sizeof(URLIO_CACHE*));
				g_cache_list[g_cache_count] = (URLIO_CACHE*) calloc(1,
						sizeof(URLIO_CACHE));
				g_cache_list[g_cache_count]->url = (char*) malloc(
						(strlen(conn->url) + 1) * sizeof(char));
				strcpy(g_cache_list[g_cache_count]->url, conn->url);
				wanted_cache_index = g_cache_count;
				g_cache_count++;
			}

			/* Expand cache memory */
			if (g_cache_list[wanted_cache_index]->bulk_count == 0) {
				g_cache_list[wanted_cache_index]->bulk_list = (char**) malloc(
						sizeof(char*));
				g_cache_list[wanted_cache_index]->bulk_id_list =
						(size_t*) malloc(sizeof(size_t));
				g_cache_list[wanted_cache_index]->bulk_size_list =
						(size_t*) malloc(sizeof(size_t));
				g_cache_list[wanted_cache_index]->bulk_list[g_cache_list[wanted_cache_index]->bulk_count] =
						(char*) malloc(want_total * sizeof(char));
			} else {
				g_cache_list[wanted_cache_index]->bulk_list = (char**) realloc(
						g_cache_list[wanted_cache_index]->bulk_list,
						(g_cache_list[wanted_cache_index]->bulk_count + 1)
								* sizeof(char*));
				g_cache_list[wanted_cache_index]->bulk_id_list =
						(size_t*) realloc(
								g_cache_list[wanted_cache_index]->bulk_id_list,
								(g_cache_list[wanted_cache_index]->bulk_count
										+ 1) * sizeof(size_t));
				g_cache_list[wanted_cache_index]->bulk_size_list =
						(size_t*) realloc(
								g_cache_list[wanted_cache_index]->bulk_size_list,
								(g_cache_list[wanted_cache_index]->bulk_count
										+ 1) * sizeof(size_t));
				g_cache_list[wanted_cache_index]->bulk_list[g_cache_list[wanted_cache_index]->bulk_count] =
						(char*) malloc(want_total * sizeof(char));
			}

			for (int t = 0; t < THREAD_NUM; t++) {
				if (pos + t * (CACHE_BULK_SIZE_PER_THREAD) > conn->size)
					break;
				/* xfer data to caller */
				memcpy(
						g_cache_list[wanted_cache_index]->bulk_list[g_cache_list[wanted_cache_index]->bulk_count]
								+ t * (CACHE_BULK_SIZE_PER_THREAD),
						conn->io[t].buffer, want_per_thread[t]);
			}
			g_cache_list[wanted_cache_index]->bulk_size_list[g_cache_list[wanted_cache_index]->bulk_count] =
					want_total;
			g_cache_list[wanted_cache_index]->bulk_id_list[g_cache_list[wanted_cache_index]->bulk_count] =
					wanted_bulk_id;

			for (int t = 0; t < THREAD_NUM; t++) {
				if (pos + t * (CACHE_BULK_SIZE_PER_THREAD) > conn->size)
					break;
				use_buffer(conn, want_per_thread[t], t);
			}
			wanted_bulk_index = g_cache_list[wanted_cache_index]->bulk_count;

			g_cache_list[wanted_cache_index]->bulk_count++;

			printf("download: obtained bulk id %zu\n", wanted_bulk_id);

			g_mutex_unlock(&g_cache_lock);
		}

		size_t copy_ptr = (i == 0) ? pos % (CACHE_BULK_SIZE) : 0;
		size_t copy_size =
				(residual
						< g_cache_list[wanted_cache_index]->bulk_size_list[wanted_bulk_index]
								- copy_ptr) ?
						residual :
						(g_cache_list[wanted_cache_index]->bulk_size_list[wanted_bulk_index]
								- copy_ptr);
		size_t ptr_ptr = wanted - residual;

		memcpy((char*) ptr + ptr_ptr,
				&g_cache_list[wanted_cache_index]->bulk_list[wanted_bulk_index][copy_ptr],
				copy_size);

		copied += copy_size;
		residual -= copy_size;
		wanted_bulk_id += CACHE_BULK_SIZE;
	}

	return copied;
}

URLIO_FILE *urlio_fopen(const char *url, const char *operation) {
	/* this code could check for URLs or types in the 'url' and
	 basically use the real fopen() for standard files */

	URLIO_FILE *file = NULL;
	URLIO_CONN *conn = NULL;
	(void) operation;
	const size_t want = 1;
	double dSize;

#ifdef URLIO_VERBOSE
	printf("fopen: %s\n", url);
#endif

	file = (URLIO_FILE*) calloc(1, sizeof(URLIO_FILE));
	if (!file)
		return NULL;

	file->handle.file = fopen(url, operation);
	if (file->handle.file) {
		file->type = CFTYPE_FILE; /* marked as FILE */

		file->url = (char*) malloc((strlen(url) + 1) * sizeof(char));
		strcpy(file->url, url);
		file->pos = 0;
		fseek(file->handle.file, 0, SEEK_END);
		file->handle.conn->size = ftell(file->handle.file);
		fseek(file->handle.file, 0, SEEK_SET);
	} else {
		file->type = CFTYPE_CURL; /* marked as URL */

		int wanted_urlio_index = -1;

		for (int i = 0; i < g_urlio_count; i++) {
			if (!strcmp(g_urlio_list[i]->url, url)) {
				printf("stream exists, reuse it\n");
				wanted_urlio_index = i;
				break;
			}
		}

		if (wanted_urlio_index == -1) {
			conn = (URLIO_CONN*) calloc(1, sizeof(URLIO_CONN));
			conn->io[0].curl = curl_easy_init();

			curl_easy_setopt(conn->io[0].curl, CURLOPT_URL, url);
			curl_easy_setopt(conn->io[0].curl, CURLOPT_WRITEDATA,
					&(conn->io[0]));
			curl_easy_setopt(conn->io[0].curl, CURLOPT_VERBOSE, CURL_VERBOSE);
			curl_easy_setopt(conn->io[0].curl, CURLOPT_WRITEFUNCTION,
					write_callback);

			conn->multi_handle = curl_multi_init();
			curl_multi_add_handle(conn->multi_handle, conn->io[0].curl);

			/* lets start the fetch */
			curl_multi_perform(conn->multi_handle, &conn->still_running);

			if ((conn->io[0].buffer_pos == 0) && (!conn->still_running)) {
				/* if still_running is 0 now, we should return NULL */

				/* make sure the easy handle is not in the multi handle anymore */
				curl_multi_remove_handle(conn->multi_handle, conn->io[0].curl);

				/* cleanup */
				curl_easy_cleanup(conn->io[0].curl);

				free(file);
				free(conn);

				file = NULL;
			} else {
				for (int retry = 0; retry < RETRY_TIMES; retry++) {
					fill_buffer(conn, want, 0);

					/* check if there's data in the buffer - if not fill either error or
					 * EOF */
					if (conn->io[0].buffer_pos) {
						use_buffer(conn, want, 0);
						curl_easy_getinfo(conn->io[0].curl,
								CURLINFO_CONTENT_LENGTH_DOWNLOAD, &dSize);
						conn->size = (long) dSize;

#ifdef URLIO_VERBOSE
						printf("stream opened, length: %zu\n", conn->size);
#endif

						break;
					}

					/* halt transaction */
					curl_multi_remove_handle(conn->multi_handle,
							conn->io[0].curl);

					/* restart */
					curl_multi_add_handle(conn->multi_handle, conn->io[0].curl);

					/* ditch buffer - write will recreate - resets stream pos*/
					free(conn->io[0].buffer);
					conn->io[0].buffer = NULL;
					conn->io[0].buffer_pos = 0;
					conn->io[0].buffer_len = 0;
				}

				/* check if there's data in the buffer - if not fill either error or
				 * EOF */
				if (conn->io[0].buffer_pos) {
					/* halt transaction */
					curl_multi_remove_handle(conn->multi_handle,
							conn->io[0].curl);

					/* restart */
					curl_multi_add_handle(conn->multi_handle, conn->io[0].curl);

					/* ditch buffer - write will recreate - resets stream pos*/
					free(conn->io[0].buffer);
					conn->io[0].buffer = NULL;
					conn->io[0].buffer_pos = 0;
					conn->io[0].buffer_len = 0;
				} else {
					/* make sure the easy handle is not in the multi handle anymore */
					curl_multi_remove_handle(conn->multi_handle,
							conn->io[0].curl);

					/* cleanup */
					curl_easy_cleanup(conn->io[0].curl);

					free(file);
					free(conn);

					file = NULL;
				}
			}

			conn->url = (char*) malloc((strlen(url) + 1) * sizeof(char));
			strcpy(conn->url, url);

			if (g_urlio_count == 0)
				g_urlio_list = (URLIO_CONN**) malloc(sizeof(URLIO_CONN*));
			else
				g_urlio_list = (URLIO_CONN**) realloc(g_urlio_list,
						(g_urlio_count + 1) * sizeof(URLIO_CONN*));
			g_urlio_list[g_urlio_count] = conn;

			wanted_urlio_index = g_urlio_count;
			g_urlio_count++;
		}

		file->handle.conn = g_urlio_list[wanted_urlio_index];
	}

	if (file) {
		file->url = (char*) malloc((strlen(url) + 1) * sizeof(char));
		strcpy(file->url, url);
		file->pos = 0;
	}

	return file;
}

int urlio_fclose(URLIO_FILE *file) {
#ifdef URLIO_VERBOSE
	printf("fclose: %s\n", file->url);
#endif
	int ret = 0; /* default is good return */

	switch (file->type) {
	case CFTYPE_FILE:
		ret = fclose(file->handle.file); /* passthrough */
		break;

	case CFTYPE_CURL:
		/* make sure the easy handle is not in the multi handle anymore */
		// curl_multi_remove_handle(file->handle.urlio->multi_handle, file->handle.curl);
		/* cleanup */
		// curl_easy_cleanup(file->handle.curl);
		break;

	default: /* unknown or supported type - oh dear */
		ret = EOF;
		errno = EBADF;
		break;
	}

	// free(file->handle.urlio->buffer);/* free any allocated buffer space */
	free(file->url);
	free(file);

	return ret;
}

int urlio_feof(URLIO_FILE *file) {
#ifdef URLIO_VERBOSE
	printf("feof: %s\n", file->url);
#endif

	int ret = 0;

	switch (file->type) {
	case CFTYPE_FILE:
		ret = feof(file->handle.file);
		break;

	case CFTYPE_CURL:
		for (int t = 0; t < THREAD_NUM; t++) {
			if ((file->handle.conn->io[t].buffer_pos == 0)
					&& (!file->handle.conn->still_running)) {
				ret = 1;

				break;
			}
		}

		break;

	default: /* unknown or supported type - oh dear */
		ret = -1;
		errno = EBADF;
		break;
	}
	return ret;
}

size_t urlio_fread(void *ptr, size_t size, size_t nmemb, URLIO_FILE *file) {
	size_t wanted;

	switch (file->type) {
	case CFTYPE_FILE:
#ifdef URLIO_VERBOSE

		printf("fread: reading %lu byte(s) from position %ld\n", size * nmemb,
				ftell(file->handle.file));
#endif

		wanted = fread(ptr, size, nmemb, file->handle.file);
		file->pos += wanted * size;
		break;

	case CFTYPE_CURL:
#ifdef URLIO_VERBOSE
		printf("fread: reading %lu byte(s) from position %ld\n", size * nmemb,
				file->pos);
#endif
		wanted = size * nmemb;
		int copied = download(ptr, file->pos, wanted, file->handle.conn);
		file->pos += copied;
		wanted = copied / size;
		break;

	default: /* unknown or supported type - oh dear */
		wanted = 0L;
		errno = EBADF;
		break;
	}

#ifdef URLIO_VERBOSE
	printf("data: ");

	for (size_t i = 0; i < (wanted * size > READ_LOG_LENGTH ?
	READ_LOG_LENGTH :
																wanted * size);
			i++)
		printf("0x%02hX ", (unsigned char) *((char*) ptr + i));

	printf("\n");
#endif

	return wanted;
}

char *urlio_fgets(char *ptr, size_t size, URLIO_FILE *file) {
	size_t wanted = size - 1; /* always need to leave room for zero termination */
	size_t loop;
	char *buf;
	size_t copied;

#ifdef URLIO_VERBOSE
	printf("fgets: from position %ld read %zu byte(s)\n", file->pos, size);
#endif

	switch (file->type) {
	case CFTYPE_FILE:
		ptr = fgets(ptr, (int) size, file->handle.file);
		file->pos += size;
		break;

//	case CFTYPE_CURL:
//		fill_buffer(file, want);
//
//		/* check if there's data in the buffer - if not fill either error or
//		 * EOF */
//		if (!file->handle.urlio->buffer_pos)
//			return NULL;
//
//		/* ensure only available data is considered */
//		if (file->handle.urlio->buffer_pos < want)
//			want = file->handle.urlio->buffer_pos;
//
//		/*buffer contains data */
//		/* look for newline or eof */
//		for (loop = 0; loop < want; loop++) {
//			if (file->handle.urlio->buffer[loop] == '\n') {
//				want = loop + 1;/* include newline */
//				break;
//			}
//		}
//
//		/* xfer data to caller */
//		memcpy(ptr, file->handle.urlio->buffer, want);
//		ptr[want] = 0; /* always null terminate */
//
//		use_buffer(file, want);
//		file->pos += size;
	case CFTYPE_CURL:
		buf = (char*) malloc(size * sizeof(char));
		copied = download(buf, file->pos, wanted, file->handle.conn);

		for (loop = 0; loop < copied; loop++) {
			if (buf[loop] == '\n') {
				copied = loop + 1;/* include newline */
				break;
			}
		}

		memcpy(ptr, buf, copied);
		free(buf);

		break;

	default: /* unknown or supported type - oh dear */
		ptr = NULL;
		errno = EBADF;
		break;
	}

#ifdef URLIO_VERBOSE
	printf("data: ");

	for (size_t i = 0L; i < (size > READ_LOG_LENGTH ? READ_LOG_LENGTH : size);
			i++)
		printf("0x%02hX ", (unsigned char) ptr[i]);

	printf("\n");
#endif

	return ptr; /*success */

}

void urlio_rewind(URLIO_FILE *file) {
	switch (file->type) {
	case CFTYPE_FILE:
#ifdef URLIO_VERBOSE
		printf("rewind: from position %ld\n", ftell(file->handle.file));
#endif

		rewind(file->handle.file); /* passthrough */
		file->pos = 0L;

		break;

	case CFTYPE_CURL:
#ifdef URLIO_VERBOSE
		printf("rewind: from position %ld\n", file->pos);
#endif
		file->pos = 0L;
		break;

	default: /* unknown or supported type - oh dear */
		break;
	}
}

int urlio_fgetc(URLIO_FILE *file) {
	// const size_t want = 1;
	int c;
	char b;

	switch (file->type) {
	case CFTYPE_FILE:
#ifdef URLIO_VERBOSE
		printf("fgetc: from position %ld read 1 byte\n",
				ftell(file->handle.file));
#endif

		c = fgetc(file->handle.file);
		file->pos++;

		break;

	case CFTYPE_CURL:
#ifdef URLIO_VERBOSE
		printf("fgetc: from position %ld read 1 byte\n", file->pos);
#endif
		download(&b, file->pos, 1, file->handle.conn);
		c = (int) b;

		file->pos++;

		break;

	default: /* unknown or supported type - oh dear */
		c = EOF;
		errno = EBADF;
		break;
	}

#ifdef URLIO_VERBOSE
	printf("data: 0x%02hX", (unsigned char) c);
#endif

	return c;/*success */
}

long int urlio_ftell(URLIO_FILE *file) {
	long int p;

	switch (file->type) {
	case CFTYPE_FILE:
		p = ftell(file->handle.file);

#ifdef URLIO_VERBOSE
		printf("ftell: current position %ld\n", p);
#endif

		break;

	case CFTYPE_CURL:
		p = file->pos;

#ifdef URLIO_VERBOSE
		printf("ftell: current position %ld\n", p);
#endif
		break;

	default: /* unknown or supported type - oh dear */
		p = -1;
		errno = EBADF;
		break;
	}

	return p; /*success */
}

int urlio_fseek(URLIO_FILE *file, long int offset, int whence) {
	switch (whence) {
	case SEEK_SET:
#ifdef URLIO_VERBOSE
		printf("fseek: seek to offset %ld from head\n", offset);
#endif
		file->pos = offset;

		break;
	case SEEK_CUR:
#ifdef URLIO_VERBOSE
		printf("fseek: seek to offset %ld from position %ld\n", offset,
				file->pos);
#endif
		file->pos = file->pos + offset;
		break;
	case SEEK_END:
#ifdef URLIO_VERBOSE
		printf("fseek: seek to offset %ld from tail\n", offset);
#endif
		file->pos = file->handle.conn->size + offset + 1;
		break;
	default: /* unknown or supported type - oh dear */
		errno = EBADF;
		return -1;
		break;
	}

	switch (file->type) {
	case CFTYPE_FILE:
		return (fseek(file->handle.file, offset, whence));
		break;

	case CFTYPE_CURL:
		return 0;

		break;

	default: /* unknown or supported type - oh dear */
		errno = EBADF;
		return 0;
		break;
	}
}

