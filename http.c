#include "http.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <fcntl.h>

extern void err_log(const char *fmt);
int read_http_hdr_request(const char *request)
{
	const char *p;
	char buf[1024], path[1024];
	int fd, i;
	
	p = request;
	while (*p && *p != '/') 
		p++;

	strcat(path, "www/");
	p++;
	for (i = 0; *p && !isspace(*p); p++, i++)
		buf[i] = *p;
	buf[i] = '\0';
	if (i == 0)
		strcat(path, "index.html");
	else
		strcat(path, buf);
	fd = open(path, O_RDONLY);
	if (fd == -1) 
		err_log("open error");

	return fd;
}

void send_http_hdr_response(int fd)
{
	
}


