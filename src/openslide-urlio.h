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

#ifndef __OPENSLIDE_URLIO_H__
#define __OPENSLIDE_URLIO_H__

#define URLIO_VERBOSE 3
#define CURL_VERBOSE 0
#define RETRY_TIMES 3
#define READ_LOG_LENGTH 8

#include <stdio.h>
#include <string.h>
#ifndef WIN32
#  include <sys/time.h>
#endif
#include <stdlib.h>
#include <errno.h>

#include <curl/curl.h>

enum fcurl_type_e {
	CFTYPE_NONE = 0, CFTYPE_FILE = 1, CFTYPE_CURL = 2
};

struct fcurl_data {
	enum fcurl_type_e type; /* type of handle */
	union {
		CURL *curl;
		FILE *file;
	} handle; /* handle */

	char *buffer; /* buffer to store cached data*/
	size_t buffer_len; /* currently allocated buffers length */
	size_t buffer_pos; /* end of data in buffer*/
	int still_running; /* Is background url fetch still in progress */

	size_t size; /* size of the stream  */
	size_t pos; /* pos of the stream  */

	char *url; /* url */
	CURLM *multi_handle;
};

typedef struct fcurl_data URLIO_FILE;

/* exported functions */
int urlio_finitial(void);
int urlio_frelease(const char *url);
int urlio_ferror(URLIO_FILE *file);

URLIO_FILE *urlio_fopen(const char *url, const char *operation);
int urlio_fclose(URLIO_FILE *file);
int urlio_feof(URLIO_FILE *file);
size_t urlio_fread(void *ptr, size_t size, size_t nmemb, URLIO_FILE *file);
char *urlio_fgets(char *ptr, size_t size, URLIO_FILE *file);
void urlio_rewind(URLIO_FILE *file);

int urlio_fgetc(URLIO_FILE *file);
long int urlio_ftell(URLIO_FILE *file);
int urlio_fseek(URLIO_FILE *file, long int offset, int whence);

#endif // __OPENSLIDE_URLIO_H__
