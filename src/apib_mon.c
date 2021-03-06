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

/*
 * This is a program that returns CPU information over the network.
 */
 
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <apr_file_io.h>
#include <apr_general.h>
#include <apr_network_io.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>

#include <apib_common.h>

#if HAVE_PTHREAD_H
#include <pthread.h>
#endif

#define LISTEN_BACKLOG 8
#define READ_BUF_LEN 128
#define PROC_BUF_LEN 512

static apr_pool_t* MainPool;

typedef struct {
  apr_socket_t* sock;
  apr_pool_t* pool;
} ThreadArgs;

static void sendBack(apr_socket_t* sock, const char* msg)
{
  apr_size_t len = strlen(msg);
  
  apr_socket_send(sock, msg, &len);
}

static int processCommand(ThreadArgs* args, const char* cmd,
			  CPUUsage* lastUsage)
{
  char buf[128];

  if (!strcasecmp(cmd, "HELLO")) {
    sendBack(args->sock, "Hi!\n");
  } else if (!strcasecmp(cmd, "CPU")) {

    double usage = cpu_GetInterval(lastUsage, args->pool);
    apr_snprintf(buf, 128, "%.2lf\n", usage);
    sendBack(args->sock, buf);

  } else if (!strcasecmp(cmd, "MEM")) {
   
    double usage = cpu_GetMemoryUsage(args->pool);
    apr_snprintf(buf, 128, "%.2lf\n", usage);
    sendBack(args->sock, buf);

  } else if (!strcasecmp(cmd, "BYE") || !strcasecmp(cmd, "QUIT")) {
    sendBack(args->sock, "BYE\n");
    return 1;
  } else {
    sendBack(args->sock, "Invalid command\n");
  }
  return 0;
}

#if HAVE_PTHREAD_CREATE
static void* SocketThread(void* a)
#else
static void* SocketThread(apr_thread_t* t, void* a)
#endif
{
  ThreadArgs* args = (ThreadArgs*)a;
  char* readBuf = apr_palloc(args->pool, READ_BUF_LEN);
  apr_status_t s;
  int closeRequested = FALSE;
  CPUUsage lastUsage;
  LineState line;

  cpu_GetUsage(&lastUsage, args->pool);
  linep_Start(&line, readBuf, READ_BUF_LEN, 0);

  while (!closeRequested) {
    s = linep_ReadSocket(&line, args->sock);
    if (s != APR_SUCCESS) {
      break;
    }
    while (!closeRequested && linep_NextLine(&line)) {
      char* l = linep_GetLine(&line);
      closeRequested = processCommand(args, l, &lastUsage);
    }
    if (!closeRequested) {
      if (linep_Reset(&line)) {
	/* Line too big to fit in buffer -- abort */
	break;
      }
    }
  }

  apr_socket_shutdown(args->sock, APR_SHUTDOWN_READWRITE);
  apr_socket_close(args->sock);
  apr_pool_destroy(args->pool);

  return NULL;
}

int main(int ac, char const* const* av)
{
  int argc = ac;
  char const* const* argv = av;
  char const * const* env = NULL;
  apr_socket_t* serverSock;
  apr_sockaddr_t* addr;
  apr_status_t s;
  char buf[128];
  
  apr_app_initialize(&argc, &argv, &env);
  apr_pool_create(&MainPool, NULL);

  if (argc != 2) {
    fprintf(stderr, "Usage: %s <port>\n", argv[0]);
    return 2;
  }

  s = apr_sockaddr_info_get(&addr, NULL, APR_INET, atoi(argv[1]), 0, MainPool);
  if (s != APR_SUCCESS) {
    goto failed;
  }

  s = apr_socket_create(&serverSock, APR_INET, SOCK_STREAM, APR_PROTO_TCP, MainPool);
  if (s != APR_SUCCESS) {
    goto failed;
  }

  s = apr_socket_opt_set(serverSock, APR_SO_REUSEADDR, TRUE);
  if (s != APR_SUCCESS) {
    goto failed;
  }

  s = apr_socket_bind(serverSock, addr);
  if (s != APR_SUCCESS) {
    goto failed;
  }

  s = apr_socket_listen(serverSock, LISTEN_BACKLOG);
  if (s != APR_SUCCESS) {
    goto failed;
  }

  cpu_Init(MainPool);

  while (TRUE) {
    apr_pool_t* socketPool;
#if HAVE_PTHREAD_CREATE
    pthread_t socketThread;
#else
    apr_thread_t* socketThread;
#endif
    apr_socket_t* clientSock;
    ThreadArgs* args;

    apr_pool_create(&socketPool, MainPool);
    s = apr_socket_accept(&clientSock, serverSock, socketPool);
    if (s != APR_SUCCESS) {
      apr_pool_destroy(socketPool);
      apr_strerror(s, buf, 128);
      fprintf(stderr, "Fatal error accepting client socket: %s\n", buf);
      continue;
    }

    args = (ThreadArgs*)apr_palloc(socketPool, sizeof(ThreadArgs));
    args->sock = clientSock;
    args->pool = socketPool;

#if HAVE_PTHREAD_CREATE
    if (pthread_create(&socketThread, NULL, SocketThread, args) == 0) {
      s = APR_SUCCESS;
    } else {
      s = -1;
    }
#else
    s = apr_thread_create(&socketThread, NULL, SocketThread, args, socketPool);
#endif
    if (s != APR_SUCCESS) { 
      apr_socket_close(clientSock);
      apr_pool_destroy(socketPool);
      apr_strerror(s, buf, 128);
      fprintf(stderr, "Fatal error creating socket thread: %s\n", buf);
      continue;
    }

#if HAVE_PTHREAD_CREATE
    pthread_detach(socketThread);
#else
    apr_thread_detach(socketThread);
#endif
  }

  apr_socket_close(serverSock);

  apr_pool_destroy(MainPool);
  apr_terminate();

  return 0;

 failed:
  apr_strerror(s, buf, 128);
  fprintf(stderr, "Fatal error: %s\n", buf);
  apr_pool_destroy(MainPool);
  apr_terminate();
  return 3;
}
