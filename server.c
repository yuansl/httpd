#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <syslog.h>

#include "http.h"

void err_log(const char *errlog);
static void http_request_handler(int connfd);
static int tcp_listen(void);
static void sig_chld(int signo);
static void daemonize(int nochdir, int noclose, const char *cmd);

int main(int argc, char *argv[])
{
	int fd, connfd;

	daemonize(0, 0, argv[0]);

	fd = tcp_listen();
	signal(SIGCHLD, sig_chld);
	for (;;) {
		connfd = accept(fd, NULL, NULL);
		if (connfd == -1)
			err_log("accept");
		if (fork() == 0) {
			close(fd);
			syslog(LOG_INFO, "A connection from client\n");
			http_request_handler(connfd);
			exit(EXIT_SUCCESS);
		}
		close(connfd);
	}
	return 0;
}

static int tcp_listen(void)
{
	struct sockaddr_in addr;
	int listenfd, opt;

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(8080);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	opt = 1;
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
	if (listenfd == -1)
		err_log("listenfd error");
	if (bind(listenfd, (struct sockaddr *) &addr, sizeof(addr)) == -1)
		err_log("bind error");
	if (listen(listenfd, 10) == -1)
		err_log("listen error");
	return listenfd;
}

static void http_request_handler(int connfd)
{
	char buf[BUFSIZ], *response;
	ssize_t n;
	int fd;

	n = read(connfd, buf, sizeof(buf));
	if (n == -1)
		err_log("read error");
	
	response = "Accept-Ranges: bytes \
Cache-Control: max-age=86400 \
Connection: Keep-Alive \
Content-Encoding: gzip \
Content-Language: en \
Content-Length: 4647 \
Content-Location: index.en.html \
Content-Type: text/html \
Date: Thu, 26 Feb 2015 05:07:49 GMT \
Etag: \"3b22-50ff1fb6839c0\" \
Expires: Fri, 27 Feb 2015 05:07:49 GMT \
Keep-Alive: timeout=5, max=100 \
Last-Modified: Wed, 25 Feb 2015 23:27:43 GMT \
Server: Apache \
TCN: choice \
Vary: negotiate,accept-language,Accept-Encoding \
";

	write(connfd, response, strlen(response));

	fd = read_http_hdr_request(buf);
	while ((n = read(fd, buf, sizeof(buf))) > 0) 
		write(connfd, buf, n);
	close(fd);
}

static void sig_chld(int signo)
{
	while (waitpid(-1, NULL, WNOHANG) != -1)
		;
}

static void daemonize(int nochdir, int noclose, const char *cmd)
{
	if (daemon(0, 0) == -1) {	/* we would get here? really? */
		perror("daemon error");
		exit(EXIT_FAILURE);
	}
	openlog(cmd, LOG_PID, LOG_DAEMON);		
}

void err_log(const char *errlog)
{
	syslog(LOG_ERR, "%s: %m", errlog);
	exit(EXIT_FAILURE);
}
