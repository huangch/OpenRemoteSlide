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

int urlio_finitial(void) {
#ifdef URLIO_VERBOSE
	printf("finitial\n");
#endif

	int ret = 0;

	return ret;
}

int urlio_frelease(const char *url) {
#ifdef URLIO_VERBOSE
	printf("frelease\n");
#endif

	int ret = 0;

	return ret;
}

int urlio_ferror(URLIO_FILE *file) {
	int ret = 0;

	switch (file->type) {
	case CFTYPE_FILE:
		ret = ferror(file->handle.file);
		break;

	case CFTYPE_CURL:
		if ((file->buffer_pos == 0) && (!file->still_running))
			ret = 1;
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

	URLIO_FILE *url = (URLIO_FILE *) userp;
	size *= nitems;

	rembuff = url->buffer_len - url->buffer_pos; /* remaining space in buffer */

	if (size > rembuff) {
		/* not enough space in buffer */
		newbuff = realloc(url->buffer, url->buffer_len + (size - rembuff));
		if (newbuff == NULL) {
			fprintf(stderr, "callback buffer grow failed\n");
			size = rembuff;
		} else {
			/* realloc succeeded increase buffer size*/
			url->buffer_len += size - rembuff;
			url->buffer = newbuff;
		}
	}

	memcpy(&url->buffer[url->buffer_pos], buffer, size);
	url->buffer_pos += size;

	return size;
}

/* use to attempt to fill the read buffer up to requested number of bytes */
static int fill_buffer(URLIO_FILE *file, size_t want) {
	fd_set fdread;
	fd_set fdwrite;
	fd_set fdexcep;
	struct timeval timeout;
	int rc;
	CURLMcode mc; /* curl_multi_fdset() return code */

	/* only attempt to fill buffer if transactions still running and buffer
	 * doesn't exceed required size already
	 */
	if ((!file->still_running) || (file->buffer_pos > want))
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

		curl_multi_timeout(file->multi_handle, &curl_timeo);
		if (curl_timeo >= 0) {
			timeout.tv_sec = curl_timeo / 1000;
			if (timeout.tv_sec > 1)
				timeout.tv_sec = 1;
			else
				timeout.tv_usec = (curl_timeo % 1000) * 1000;
		}

		/* get file descriptors from the transfers */
		mc = curl_multi_fdset(file->multi_handle, &fdread, &fdwrite, &fdexcep,
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
			curl_multi_perform(file->multi_handle, &file->still_running);
			break;
		}
	} while (file->still_running && (file->buffer_pos < want));
	return 1;
}

/* use to remove want bytes from the front of a files buffer */
static int use_buffer(URLIO_FILE *file, size_t want) {
	/* sort out buffer */
	if ((file->buffer_pos - want) <= 0) {
		/* ditch buffer - write will recreate */
		free(file->buffer);
		file->buffer = NULL;
		file->buffer_pos = 0;
		file->buffer_len = 0;
	} else {
		/* move rest down make it available for later */
		memmove(file->buffer, &file->buffer[want], (file->buffer_pos - want));

		file->buffer_pos -= want;
	}
	return 0;
}

URLIO_FILE *urlio_fopen(const char *url, const char *operation) {
	/* this code could check for URLs or types in the 'url' and
	 basically use the real fopen() for standard files */

	URLIO_FILE *file;
	(void) operation;
	const size_t want = 1;
	double dSize;

	file = (URLIO_FILE*)calloc(1, sizeof(URLIO_FILE));
	if (!file)
		return NULL;

	file->handle.file = fopen(url, operation);
	if (file->handle.file) {
		file->type = CFTYPE_FILE; /* marked as FILE */

		file->url = (char*) malloc((strlen(url)+1) * sizeof(char));
		strcpy(file->url, url);
		file->pos = 0;
		fseek(file->handle.file, 0, SEEK_END);
		file->size = ftell(file->handle.file);
		fseek(file->handle.file, 0, SEEK_SET);
	} else {
		file->type = CFTYPE_CURL; /* marked as URL */
		file->handle.curl = curl_easy_init();

		curl_easy_setopt(file->handle.curl, CURLOPT_URL, url);
		curl_easy_setopt(file->handle.curl, CURLOPT_WRITEDATA, file);
		curl_easy_setopt(file->handle.curl, CURLOPT_VERBOSE, CURL_VERBOSE);
		curl_easy_setopt(file->handle.curl, CURLOPT_WRITEFUNCTION,
				write_callback);

		if (!file->multi_handle)
			file->multi_handle = curl_multi_init();

		curl_multi_add_handle(file->multi_handle, file->handle.curl);

		/* lets start the fetch */
		curl_multi_perform(file->multi_handle, &file->still_running);

		if ((file->buffer_pos == 0) && (!file->still_running)) {
			/* if still_running is 0 now, we should return NULL */

			/* make sure the easy handle is not in the multi handle anymore */
			curl_multi_remove_handle(file->multi_handle, file->handle.curl);

			/* cleanup */
			curl_easy_cleanup(file->handle.curl);

			free(file);

			file = NULL;
		} else {
			for (int retry = 0; retry < RETRY_TIMES; retry++) {
				fill_buffer(file, want);

				/* check if there's data in the buffer - if not fill either errored or
				 * EOF */
				if (file->buffer_pos) {
					use_buffer(file, want);

					curl_easy_getinfo(file->handle.curl,
							CURLINFO_CONTENT_LENGTH_DOWNLOAD, &dSize);
					file->size = (long) dSize;

					break;
				}

				/* halt transaction */
				curl_multi_remove_handle(file->multi_handle, file->handle.curl);

				/* restart */
				curl_multi_add_handle(file->multi_handle, file->handle.curl);

				/* ditch buffer - write will recreate - resets stream pos*/
				free(file->buffer);
				file->buffer = NULL;
				file->buffer_pos = 0;
				file->buffer_len = 0;
			}

			/* check if there's data in the buffer - if not fill either errored or
			 * EOF */
			if (file->buffer_pos) {
				/* halt transaction */
				curl_multi_remove_handle(file->multi_handle, file->handle.curl);

				/* restart */
				curl_multi_add_handle(file->multi_handle, file->handle.curl);

				/* ditch buffer - write will recreate - resets stream pos*/
				free(file->buffer);
				file->buffer = NULL;
				file->buffer_pos = 0;
				file->buffer_len = 0;
			} else {
				/* make sure the easy handle is not in the multi handle anymore */
				curl_multi_remove_handle(file->multi_handle, file->handle.curl);

				/* cleanup */
				curl_easy_cleanup(file->handle.curl);

				free(file);

				file = NULL;
			}
		}
	}

	if(file) {
		file->url = (char*) malloc((strlen(url)+1) * sizeof(char));
		strcpy(file->url, url);

		file->pos = 0;

#ifdef URLIO_VERBOSE
	printf("fopen: %s\nstream length: %zu\n", url, file->size);
#endif
	}

	return file;
}

int urlio_fclose(URLIO_FILE *file) {
#ifdef URLIO_VERBOSE
	printf("fclose: %s\n", file->url);
#endif
	int ret = 0;/* default is good return */

	switch (file->type) {
	case CFTYPE_FILE:
		ret = fclose(file->handle.file); /* passthrough */
		break;

	case CFTYPE_CURL:
		/* make sure the easy handle is not in the multi handle anymore */
		curl_multi_remove_handle(file->multi_handle, file->handle.curl);

		/* cleanup */
		curl_easy_cleanup(file->handle.curl);
		break;

	default: /* unknown or supported type - oh dear */
		ret = EOF;
		errno = EBADF;
		break;
	}

	free(file->buffer);/* free any allocated buffer space */
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
		if ((file->buffer_pos == 0) && (!file->still_running))
			ret = 1;
		break;

	default: /* unknown or supported type - oh dear */
		ret = -1;
		errno = EBADF;
		break;
	}
	return ret;
}

size_t urlio_fread(void *ptr, size_t size, size_t nmemb, URLIO_FILE *file) {
	size_t want;

	switch (file->type) {
	case CFTYPE_FILE:
#ifdef URLIO_VERBOSE
		printf("fread: reading %lu byte(s) from position %ld\n", size*nmemb, ftell(file->handle.file));
#endif

		want = fread(ptr, size, nmemb, file->handle.file);
		file->pos += want * size;
		break;

	case CFTYPE_CURL:
#ifdef URLIO_VERBOSE
		printf("fread: reading %lu byte(s) from position %ld\n", size*nmemb, file->pos);
#endif

		want = nmemb * size;

		fill_buffer(file, want);

		/* check if there's data in the buffer - if not fill_buffer()
		 * either errored or EOF */
		if (!file->buffer_pos)
			return 0;

		/* ensure only available data is considered */
		if (file->buffer_pos < want)
			want = file->buffer_pos;

		/* xfer data to caller */
		memcpy(ptr, file->buffer, want);

		use_buffer(file, want);
		file->pos += want;
		want = want / size; /* number of items */
		break;

	default: /* unknown or supported type - oh dear */
		want = 0;
		errno = EBADF;
		break;

	}

#ifdef URLIO_VERBOSE
	printf("data: ");

	for(size_t i = 0; i < (want * size > READ_LOG_LENGTH? READ_LOG_LENGTH: want * size); i ++)
		printf("0x%02hX ", (unsigned char)*((char*)ptr+i));

	printf("\n");
#endif

	return want;
}

char *urlio_fgets(char *ptr, size_t size, URLIO_FILE *file) {
	size_t want = size - 1; /* always need to leave room for zero termination */
	size_t loop;

#ifdef URLIO_VERBOSE
		printf("fgets: from position %ld read %zu byte(s)\n", file->pos, size);
#endif

	switch (file->type) {
	case CFTYPE_FILE:
		ptr = fgets(ptr, (int) size, file->handle.file);
		file->pos += size;
		break;

	case CFTYPE_CURL:
		fill_buffer(file, want);

		/* check if there's data in the buffer - if not fill either errored or
		 * EOF */
		if (!file->buffer_pos)
			return NULL;

		/* ensure only available data is considered */
		if (file->buffer_pos < want)
			want = file->buffer_pos;

		/*buffer contains data */
		/* look for newline or eof */
		for (loop = 0; loop < want; loop++) {
			if (file->buffer[loop] == '\n') {
				want = loop + 1;/* include newline */
				break;
			}
		}

		/* xfer data to caller */
		memcpy(ptr, file->buffer, want);
		ptr[want] = 0;/* always null terminate */

		use_buffer(file, want);
		file->pos += size;

		break;

	default: /* unknown or supported type - oh dear */
		ptr = NULL;
		errno = EBADF;
		break;
	}

#ifdef URLIO_VERBOSE
	printf("data: ");

	for(size_t i = 0; i < (size > READ_LOG_LENGTH? READ_LOG_LENGTH: size); i ++)
		printf("0x%02hX ", (unsigned char)ptr[i]);

	printf("\n");
#endif

	return ptr;/*success */

}

void urlio_rewind(URLIO_FILE *file) {
	switch (file->type) {
	case CFTYPE_FILE:
#ifdef URLIO_VERBOSE
		printf("rewind: from position %ld\n", ftell(file->handle.file));
#endif

		rewind(file->handle.file); /* passthrough */
		file->pos = 0;

		break;

	case CFTYPE_CURL:
#ifdef URLIO_VERBOSE
		printf("rewind: from position %ld\n", file->pos);
#endif

		/* halt transaction */
		curl_multi_remove_handle(file->multi_handle, file->handle.curl);

		/* restart */
		curl_multi_add_handle(file->multi_handle, file->handle.curl);

		/* ditch buffer - write will recreate - resets stream pos*/
		free(file->buffer);
		file->buffer = NULL;
		file->buffer_pos = 0;
		file->buffer_len = 0;

		file->pos = 0;
		break;

	default: /* unknown or supported type - oh dear */
		break;
	}
}

int urlio_fgetc(URLIO_FILE *file) {
	const size_t want = 1;
	int c;

	switch (file->type) {
	case CFTYPE_FILE:
#ifdef URLIO_VERBOSE
		printf("fgetc: from position %ld read 1 byte\n", ftell(file->handle.file));
#endif

		c = fgetc(file->handle.file);
		file->pos ++;

		break;

	case CFTYPE_CURL:
#ifdef URLIO_VERBOSE
		printf("fgetc: from position %ld read 1 byte\n", file->pos);
#endif

		fill_buffer(file, want);

		/* check if there's data in the buffer - if not fill either errored or
		 * EOF */
		if (!file->buffer_pos)
			return EOF;

		/* xfer data to caller */
		c = file->buffer[0];

		use_buffer(file, want);

		file->pos ++;

		break;

	default: /* unknown or supported type - oh dear */
		c = EOF;
		errno = EBADF;
		break;
	}

#ifdef URLIO_VERBOSE
	printf("data: 0x%02hX", (unsigned char)c);
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

		if (offset == 0) {
			printf("debug");

		}

		break;
	case SEEK_CUR:
#ifdef URLIO_VERBOSE
		printf("fseek: seek to offset %ld from position %ld\n", offset, file->pos);
#endif
		file->pos = file->pos + offset;
		break;
	case SEEK_END:
#ifdef URLIO_VERBOSE
		printf("fseek: seek to offset %ld from tail\n", offset);
#endif
		file->pos = file->size + offset + 1;
		break;
	default: /* unknown or supported type - oh dear */
		errno = EBADF;
		return(-1);
		break;
	}

	switch (file->type) {
	case CFTYPE_FILE:
		return(fseek(file->handle.file, offset, whence));
		break;

	case CFTYPE_CURL:
		/* close the stream */
		/* make sure the easy handle is not in the multi handle anymore */
		curl_multi_remove_handle(file->multi_handle, file->handle.curl);

		/* cleanup */
		curl_easy_cleanup(file->handle.curl);

		/* reopen the stream and resume at a specific pos */
		file->type = CFTYPE_CURL; /* marked as URL */
		file->handle.curl = curl_easy_init();

		curl_easy_setopt(file->handle.curl, CURLOPT_URL, file->url);
		curl_easy_setopt(file->handle.curl, CURLOPT_WRITEDATA, file);
		curl_easy_setopt(file->handle.curl, CURLOPT_VERBOSE, CURL_VERBOSE);
		curl_easy_setopt(file->handle.curl, CURLOPT_WRITEFUNCTION,
				write_callback);
		curl_easy_setopt(file->handle.curl, CURLOPT_RESUME_FROM_LARGE,
				file->pos);

		if (!file->multi_handle)
			file->multi_handle = curl_multi_init();

		curl_multi_add_handle(file->multi_handle, file->handle.curl);

		/* lets start the fetch */
		curl_multi_perform(file->multi_handle, &file->still_running);

		if ((file->buffer_pos == 0) && (!file->still_running)) {
			/* if still_running is 0 now, we should return NULL */

			/* make sure the easy handle is not in the multi handle anymore */
			curl_multi_remove_handle(file->multi_handle, file->handle.curl);

			/* cleanup */
			curl_easy_cleanup(file->handle.curl);

			return(-1);
		} else {
			/* check if there's data in the buffer - if not fill either errored or
			 * EOF */
			if (file->buffer_pos) {
				/* halt transaction */
				curl_multi_remove_handle(file->multi_handle, file->handle.curl);

				/* restart */
				curl_multi_add_handle(file->multi_handle, file->handle.curl);

				/* ditch buffer - write will recreate - resets stream pos*/
				free(file->buffer);
				file->buffer = NULL;
				file->buffer_pos = 0;
				file->buffer_len = 0;
			}
		}

		return(0);

		break;

	default: /* unknown or supported type - oh dear */
		errno = EBADF;
		return(0);
		break;
	}
}

