#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <netdb.h>

void err_sys(const char *msg);
void err_exit(const char *fmt, ...);
int tcp_connect(const char *host);
void http_response_handler(int sockfd);
void http_request_handler(int sockfd, const char *method,
				 const char *url, const char *host);
void pump(int sockfd, const char *buf, ssize_t size);
void process_whole_line(const char *line);
void process_statusline(const char *line);
void process_headers(const char *line);
void flush_headers(void);
void begin_body(void);
void process_chunklen(const char *line);
size_t process_chunked_data(const char *line, size_t size);
size_t process_nonchunked_data(const char *line, size_t size);
void process_trailers(const char *line);

#endif
