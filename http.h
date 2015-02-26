#ifndef HTTP_H
#define HTTP_H

struct httphdr_request {
	char *host;
	char *user_agent;
	char *accept;
	char *accept_language;
	char *accpet_encoding;
	char *connection;
};

struct httphdr_response {
	char *accept_ranges;
	char *connection;
	char *content_type;
	char *date;
	char *last_modified;
	char *server;
};

int read_http_hdr_request(const char *request);
void send_http_hdr_response(int fd);

#endif
