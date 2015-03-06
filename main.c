#include "httpclient.h"

int main(int argc, char *argv[])
{
	int sockfd;
	const char *url, *host;

	if (argc < 2 || argc > 3)
		err_exit("Usage: client Host [url]\n");

	sockfd = tcp_connect(argv[1]);
	host = argv[1];
	url = argv[2];
	http_request_handler(sockfd, "GET", url, host);
	http_response_handler(sockfd);
	return 0;
}
