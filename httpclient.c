#include "httpclient.h"
#include "map.h"
#include <stdbool.h>
#include <ctype.h>
#include <assert.h>

/* http status code 
 * borrowed from "Ben Campbell: happyhttp.h"
*/

enum http_status_code {
	// 1xx informational
	CONTINUE = 100,
	SWITCHING_PROTOCOLS = 101,
	PROCESSING = 102,

	// 2xx successful
	OK = 200,
	CREATED = 201,
	ACCEPTED = 202,
	NON_AUTHORITATIVE_INFORMATION = 203,
	NO_CONTENT = 204,
	RESET_CONTENT = 205,
	PARTIAL_CONTENT = 206,
	MULTI_STATUS = 207,
	IM_USED = 226,

	// 3xx redirection
	MULTIPLE_CHOICES = 300,
	MOVED_PERMANENTLY = 301,
	FOUND = 302,
	SEE_OTHER = 303,
	NOT_MODIFIED = 304,
	USE_PROXY = 305,
	TEMPORARY_REDIRECT = 307,
	
	// 4xx client error
	BAD_REQUEST = 400,
	UNAUTHORIZED = 401,
	PAYMENT_REQUIRED = 402,
	FORBIDDEN = 403,
	NOT_FOUND = 404,
	METHOD_NOT_ALLOWED = 405,
	NOT_ACCEPTABLE = 406,
	PROXY_AUTHENTICATION_REQUIRED = 407,
	REQUEST_TIMEOUT = 408,
	CONFLICT = 409,
	GONE = 410,
	LENGTH_REQUIRED = 411,
	PRECONDITION_FAILED = 412,
	REQUEST_ENTITY_TOO_LARGE = 413,
	REQUEST_URI_TOO_LONG = 414,
	UNSUPPORTED_MEDIA_TYPE = 415,
	REQUESTED_RANGE_NOT_SATISFIABLE = 416,
	EXPECTATION_FAILED = 417,
	UNPROCESSABLE_ENTITY = 422,
	LOCKED = 423,
	FAILED_DEPENDENCY = 424,
	UPGRADE_REQUIRED = 426,

	// 5xx server error
	INTERNAL_SERVER_ERROR = 500,
	NOT_IMPLEMENTED = 501,
	BAD_GATEWAY = 502,
	SERVICE_UNAVAILABLE = 503,
	GATEWAY_TIMEOUT = 504,
	HTTP_VERSION_NOT_SUPPORTED = 505,
	INSUFFICIENT_STORAGE = 507,
	NOT_EXTENDED = 510,
};

#define MAXLINE 2048
static void response_init(void);

int tcp_connect(const char *host)
{
	struct addrinfo hints, *res, *pres;
	int sockfd;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	if (getaddrinfo(host, "http", &hints, &res) != 0)
		err_exit("getaddrinfo: %s\n", gai_strerror(h_errno));

	for (pres = res; pres; pres = pres->ai_next) {
		sockfd = socket(pres->ai_family, pres->ai_socktype,
			pres->ai_protocol);
		if (sockfd == -1)
			continue;
		if (connect(sockfd, pres->ai_addr, pres->ai_addrlen) == 0)
			break;
		close(sockfd);
	}
	if (pres == NULL)
		err_sys("connect error");
	freeaddrinfo(res);
	return sockfd;
}

void http_request_handler(int sockfd, const char *method,
				 const char *url, const char *host)
{
	char req_data[BUFSIZ];

	if (url == NULL)
		url = "/";
	sprintf(req_data,
		"%s %s HTTP/1.1\r\n"
		"Host: %s\r\n"		
		"User-Agent: curl/7.35.0\r\n"
		"Accept-Encoding: identity\r\n"
		"\r\n", method, url, host);
	if (write(sockfd, req_data, strlen(req_data)) == -1)
		err_sys("write error");
}

/* handles parsing of response data. borrowed from "Ben " */
enum response_state {
	STATUSLINE,
	HEADERS,
	CHUNKLEN,	/* expecting a chunk length indicator (in hex) */
	CHUNKEND,
	BODY,
	TRAILERS,       /* trailers after body */
	COMPLETE,
} m_state;

void http_response_handler(int sockfd)
{
	ssize_t nread;
	char buf[BUFSIZ];
	int nready;	
	fd_set rset;

	response_init();	
	FD_ZERO(&rset);
	for (;;) {
		FD_SET(sockfd, &rset);
		nready = select(sockfd + 1, &rset, NULL, NULL, NULL);
		if (nready <= 0) {
			if (nready == -1 && errno == EINTR)
				continue;
			else if (nready == -1)
				err_sys("select error");
			fprintf(stderr, "timeout for select\n");
			continue;
		}
		if (FD_ISSET(sockfd, &rset)) {
			nread = read(sockfd, buf, sizeof(buf));
			if (nread == 0)
				break;
			else if (nread == -1)
				err_sys("read error");
			pump(sockfd, buf, nread);
			if (m_state == TRAILERS || m_state == COMPLETE)
				break;
		}
	}
	close(sockfd);
}

static ssize_t chunkleft = 0, content_length = 0;
struct map map = { { 0, "" }, { 0, "" }, NULL, NULL };
static char header_accum[2048];
int stat_code;
bool m_chunked;

static void response_init(void)
{
	m_state = STATUSLINE;
	m_chunked = false;
}

void pump(int sockfd, const char *data, ssize_t size)
{
	int i, ch;
	ssize_t used, count;
	static char line[MAXLINE];

	count = size;
	while (count > 0 && m_state != COMPLETE) {
		if (m_state != BODY) {
			i = 0;
			while (count-- > 0) {
				ch = *data++;
				if (ch == '\n') {
					line[i] = '\0';
					process_whole_line(line);
					break;
				}
				if (ch != '\r') 
					line[i++] = ch;
			}
		} else {
			if (m_chunked) {
				if (count > chunkleft)
					size = chunkleft;
				else
					size = count;
				used = process_chunked_data(data, size);
			} else {
				if (count > content_length)
					size = content_length;
				else
					size = count;
				used = process_nonchunked_data(data, size);
			}
			data += used;
			count -= used;
			while (count-- > 0 && *data == '\n') {
				putchar('\n');
				data++;
			}
		}
		if (m_state == TRAILERS)
			close(sockfd);
	}
}

void process_whole_line(const char *line)
{
	switch (m_state) {
	case STATUSLINE:
		process_statusline(line);
		break;
	case HEADERS:
		process_headers(line);
		break;
	case CHUNKLEN:
		process_chunklen(line);
		break;
	case TRAILERS:
		process_trailers(line);
		break;
	case CHUNKEND:
		assert(m_chunked);
		m_state = CHUNKLEN;
		break;
	default:
		break;
	}
}

void process_statusline(const char *line)
{
	const char *p = line;
	static int i;
	char status_code[4]; /* status code: 1XX ~ 5XX */
	char http_version[10], reason[100];    /* status reason: e.g 200 OK */

	while (*p && isspace(*p)) 
		putchar(*p++);
	i = 0;
	while (*p && !isspace(*p))
		http_version[i++] = *p++;
	http_version[i] = '\0';

	while (*p && isspace(*++p))
		;
	i = 0;
	while (*p && !isspace(*p))
		status_code[i++] = *p++;
	status_code[i] = '\0';
	stat_code = atoi(status_code);
	while (*p && isspace(*++p))
		;
	strcpy(reason, p);
	printf("%s %s %s\n", http_version, status_code, reason);
	m_state = HEADERS;
}

void process_headers(const char *line)
{
	const char *p = line;
	
	if (line[0] == '\0') {
		flush_headers();
		if (stat_code == CONTINUE)
			m_state = STATUSLINE;
		else
			begin_body();
		return;
	}

	if (isspace(*p)) { /* continous line */
		while (*p && isspace(*++p))
			;
		strcat(header_accum, " ");
		strcat(header_accum, p);
	} else {
		flush_headers();
		strcpy(header_accum, p);
	}
}

void flush_headers(void)
{
	char header[128], value[1024], *p;
	int i;
	
	p = header_accum;
	if (p[0] == '\0')
		return;
	
	for (i = 0; *p && *p != ':'; i++, p++)
		header[i] = tolower(*p);
	header[i] = '\0';

	if (*p == ':')
		p++;	/* skip ':' */
	while (*p && isspace(*p))
		p++;
	strncpy(value, p, strlen(p) + 1);
	map_insert(&map, header, value);
	printf("%s: %s\n", header, value);
	header_accum[0] = '\0';
}

void begin_body(void)
{
	const char *p;

	if (100 <= stat_code && stat_code < 200) {
		m_state = TRAILERS;  /* This response has no body part */
		return;
	}
	p = map_at(&map, "transfer-encoding");
	if (p && strcasecmp(p, "chunked") == 0) {
		m_chunked = true;
		m_state = CHUNKLEN;
		return;
	}
	p = map_at(&map, "content-length");
	assert(p != NULL);
	content_length = atoi(p);
	m_state = BODY;
}

void process_chunklen(const char *line)
{
	chunkleft = strtol(line, NULL, 16);
	if (chunkleft == 0) {
		m_state = TRAILERS;
		return;
	}
	m_state = BODY;
}

size_t process_chunked_data(const char *data, size_t size)
{
	static size_t used;

	if ((used = write(STDOUT_FILENO, data, size)) != size)
		err_sys("write error");

	chunkleft -= used;
	assert(chunkleft >= 0);
	if (chunkleft == 0) 
		m_state = CHUNKEND;
	return used;
}

size_t process_nonchunked_data(const char *data, size_t size)
{
	ssize_t used;

	if ((used = write(1, data, size)) != size)
		err_sys("write error");
	content_length -= used;
	assert(content_length >= 0);
	if (content_length == 0)
		m_state = TRAILERS;
	return used;
}

void process_trailers(const char *line)
{
	m_state = COMPLETE;
}

void err_sys(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

void err_exit(const char *fmt, ...)
{
	va_list ap;
	char buf[BUFSIZ];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	fputs(buf, stderr);
	va_end(ap);
	exit(EXIT_FAILURE);
}
