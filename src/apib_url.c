/*
   Copyright 2013 Apigee Corp.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>

#include <apib_common.h>

#define URL_BUF_LEN 8192
#define INITIAL_URLS 16
#define MAX_ENTROPY_ROUNDS 100
#define SEED_SIZE 8192

static unsigned int urlCount = 0;
static unsigned int urlSize = 0;
static URLInfo*     urls;

const URLInfo* url_GetNext(RandState rand)
{
  long randVal;

  if (urlCount == 0) {
    return NULL;
  }
  if (urlCount == 1) {
    return &(urls[0]);
  }

#if HAVE_LRAND48_R
  lrand48_r(rand, &randVal);
#elif HAVE_RAND_R
  randVal = rand_r(rand);
#else
  randVal = rand();
#endif

  return &(urls[randVal % urlCount]);
}

static int initUrl(URLInfo* u, apr_pool_t* pool)
{
  apr_status_t s;
  char errBuf[128];
  apr_sockaddr_t* newAddrs;
  apr_sockaddr_t* a;
  int addrCount;

  if (u->url.scheme == NULL) {
    fprintf(stderr, "Missing URL scheme\n");
    return -1;
  } else if (!strcmp(u->url.scheme, "https")) {
    u->isSsl = TRUE;
  } else if (!strcmp(u->url.scheme, "http")) {
    u->isSsl = FALSE;
  } else {
    fprintf(stderr, "Invalid URL scheme\n");
    return -1;
  }

  if (u->url.port_str == NULL) {
    if (u->isSsl) {
      u->port = 443;
    } else {
      u->port = 80;
    }
  } else {
    u->port = atoi(u->url.port_str);
  }

  s = apr_sockaddr_info_get(&newAddrs, u->url.hostname,
			    APR_INET, u->port, 0, pool);
  if (s != APR_SUCCESS) {
    apr_strerror(s, errBuf, 128);
    fprintf(stderr, "Error looking up host \"%s\": %s\n", 
	    u->url.hostname, errBuf);
    return -1;
  }

  addrCount = 0;
  a = newAddrs;
  while (a != NULL) {
    addrCount++;
    a = a->next;
  }

  u->addressCount = addrCount;
  u->addresses = (apr_sockaddr_t**)apr_palloc(pool, sizeof(apr_sockaddr_t*) * addrCount);
  a = newAddrs;
  for (int i = 0; i < addrCount; i++) {
    u->addresses[i] = a;
    a = a->next;
  }

  return 0;
}

static apr_sockaddr_t* getConn(const URLInfo* u, int index) 
{
  return u->addresses[index % u->addressCount];
}

int url_InitOne(const char* urlStr, apr_pool_t* pool)
{
  apr_status_t s;

  urlCount = urlSize = 1;
  urls = (URLInfo*)malloc(sizeof(URLInfo));

  s = apr_uri_parse(pool, urlStr, &(urls[0].url));
  if (s != APR_SUCCESS) {
    fprintf(stderr, "Invalid URL\n");
    return -1;
  }

  return initUrl(&(urls[0]), pool);
}

apr_sockaddr_t* url_GetAddress(const URLInfo* url, int index)
{
  apr_sockaddr_t* a = getConn(url, index);

#if DEBUG
  char* str;
  apr_sockaddr_ip_get(&str, a);
  printf("Connecting to %s\n", str);
#endif
  return a;
}

int url_IsSameServer(const URLInfo* u1, const URLInfo* u2, int index)
{
  if (u1->port != u2->port) {
    return 0;
  }
  return apr_sockaddr_equal(getConn(u1, index), 
                            getConn(u2, index));
}

int url_InitFile(const char* fileName, apr_pool_t* pool)
{
  apr_status_t s;
  apr_file_t* file;
  char buf[URL_BUF_LEN];
  LineState line;

  urlCount = 0;
  urlSize = INITIAL_URLS;
  urls = (URLInfo*)malloc(sizeof(URLInfo) * INITIAL_URLS);

  s = apr_file_open(&file, fileName, APR_READ, APR_OS_DEFAULT, pool);
  if (s != APR_SUCCESS) {
    fprintf(stderr, "Can't open \"%s\"\n", fileName);
    return -1;
  }

  linep_Start(&line, buf, URL_BUF_LEN, 0);
  s = linep_ReadFile(&line, file);
  if (s != APR_SUCCESS) {
    apr_file_close(file);
    return -1;
  }

  do {
    while (linep_NextLine(&line)) {
      char* urlStr = linep_GetLine(&line);
      if (urlCount == urlSize) {
	urlSize *= 2;
	urls = (URLInfo*)realloc(urls, sizeof(URLInfo) * urlSize);
      }
      s = apr_uri_parse(pool, urlStr, &(urls[urlCount].url));
      if (s != APR_SUCCESS) {
	fprintf(stderr, "Invalid URL \"%s\"\n", urlStr);
	apr_file_close(file);
	return -1;
      }
      if (initUrl(&(urls[urlCount]), pool) != 0) {
	apr_file_close(file);
	return -1;
      }
      urlCount++;
    }
    linep_Reset(&line);
    s = linep_ReadFile(&line, file);
  } while (s == APR_SUCCESS);

  printf("Read %i URLs from \"%s\"\n", urlCount, fileName);

  apr_file_close(file);
  return 0;
}

void url_InitRandom(RandState state)
{
  unsigned int seed;

  apr_generate_random_bytes((unsigned char*)&seed, sizeof(int));

#if HAVE_LRAND48_R
  srand48_r(seed, state);
#else
  srand(seed);
#endif
}

