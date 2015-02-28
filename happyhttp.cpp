/*
 * HappyHTTP - a simple HTTP library
 * Version 0.1
 * 
 * Copyright (c) 2006 Ben Campbell
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 * 
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not
 * be misrepresented as being the original software.
 * 
 * 3. This notice may not be removed or altered from any source distribution.
 *
 */

#include "happyhttp.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>	// for gethostbyname() but we should use getaddrinfo instead
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdarg>

#include <string>
#include <vector>
#include <algorithm>
#include <iostream>

using namespace std;

namespace happyhttp
{
	static void err_exit(const char *fmt, ...)
	{
		va_list ap;
		static char buf[512];
		static int n;

		va_start(ap, fmt);
		n = ::vsnprintf(buf, sizeof(buf), fmt, ap);
		if (n == sizeof(buf))
			buf[n - 1] = '\0';
		fputs(buf, stderr);
		va_end(ap);
		exit(EXIT_FAILURE);
	}

// return true if socket has data waiting to be read
	bool datawaiting(int sockfd)
	{
		fd_set rset;
		int flag;

		flag = fcntl(sockfd, F_GETFL, 0);
		fcntl(sockfd, F_SETFL, flag | O_NONBLOCK);
		FD_ZERO(&rset);
		FD_SET(sockfd, &rset);

		int nready = select(sockfd + 1, &rset, NULL, NULL, NULL);
		if (nready == -1)
			err_exit("select");

		if(FD_ISSET(sockfd, &rset))
			return true;
		else
			return false;
	}

//---------------------------------------------------------------------
//
// Connection
//
//---------------------------------------------------------------------
	void connection_init(Connection *conn, const char *host, int port)
	{
		conn->m_ResponseBeginCB = NULL;
		conn->m_ResponseDataCB = NULL; 
		conn->m_ResponseCompleteCB = NULL;
		conn->m_UserData = NULL;
		conn->m_State = IDLE;
		conn->m_Host = host;
		conn->m_Port = port;
		conn->m_Sock = -1;
	}

	void connection_destroy(Connection *conn)
	{
		close(conn);
	}
	
	void setcallbacks(Connection *conn, ResponseBegin_CB begincb,
				      ResponseData_CB datacb,
				      ResponseComplete_CB completecb, void *userdata)
	{
		conn->m_ResponseBeginCB = begincb;
		conn->m_ResponseDataCB = datacb;
		conn->m_ResponseCompleteCB = completecb;
		conn->m_UserData = userdata;
	}
	
	// any requests still outstanding?
	bool outstanding(Connection *conn) 
	{
		return !conn->m_Outstanding.empty();
	}
	
	void pump(Connection *conn)
	{
		if (conn->m_Outstanding.empty())
			return;		// no requests outstanding

		Response *r;
		assert(conn->m_Sock > 0); // outstanding requests but no connection!

		if (!datawaiting(conn->m_Sock))
			return;				// recv will block

		unsigned char buf[2048];
		ssize_t n = recv(conn->m_Sock, buf, sizeof(buf), 0);

		if (n < 0)
			err_exit("recv error: %s\n", strerror(errno));

		if (n == 0) {
			// connection has closed
			r = conn->m_Outstanding.front();
			notifyconnectionclosed(r);
			assert(completed(r));
			delete r;
			conn->m_Outstanding.pop_front();

			// any outstanding requests will be discarded
			close(conn);
		} else {
			int used = 0;
			while (used < n && !conn->m_Outstanding.empty()) {
				r = conn->m_Outstanding.front();
				int u = pump(r, buf + used, n - used);

				// delete response once completed
				
				if (completed(r)) {
					delete r;
					conn->m_Outstanding.pop_front();
				}
				used += u;
			}

			// NOTE: will lose bytes if response queue goes empty
			// (but server shouldn't be sending anything if we don't have
			// anything outstanding anyway)
			assert(used == n);	// all bytes should be used up by here.
		}
	}
	
	void tcp_connect(Connection *conn)
	{
		struct addrinfo hints, *res, *pres;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = 0;

		if (getaddrinfo(conn->m_Host.c_str(), "http", &hints, &res) != 0) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(h_errno));
			exit(EXIT_FAILURE);
		}
		
		for (pres = res; pres; pres = pres->ai_next) {
			conn->m_Sock = socket(pres->ai_family, pres->ai_socktype,
				pres->ai_protocol);
			if (conn->m_Sock == -1)
				continue;
			if (connect(conn->m_Sock, pres->ai_addr, pres->ai_addrlen) == 0)
				break;
			::close(conn->m_Sock);
		}
		if (pres == NULL) {
			fprintf(stderr, "connect error: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		freeaddrinfo(res);
	}

	void close(Connection *conn)
	{
		::close(conn->m_Sock);
		conn->m_Sock = -1;
		// discard any incomplete responses
		while (!conn->m_Outstanding.empty()) {
			delete conn->m_Outstanding.front();
			conn->m_Outstanding.pop_front();
		}
	}


	void request(Connection *conn, const char *method, const char *url,
				 const char *headers[], const unsigned char *body,
				 int bodysize)
	{
		bool gotcontentlength = false;	// already in headers?

		// check headers for content-length
		// TODO: check for "Host" and "Accept-Encoding" too
		// and avoid adding them ourselves in putrequest()
		if (headers) {
			const char **h = headers;
			while (*h) {
				const char* name = *h++;
				const char* value = *h++;
				assert (value != 0);	// name with no value!
				if (!strcasecmp(name, "content-length"))
					gotcontentlength = true;
			}
		}

		putrequest(conn, method, url);
		if (body != NULL && !gotcontentlength)
			putheader(conn, "Content-Length", bodysize);
		if (headers) {
			const char **h = headers;
			while (*h) {
				const char *name = *h++;
				const char *value = *h++;
				putheader(conn, name, value);
			}
		}
		endheaders(conn);
		if (body)
			send(conn, (const char *) body, bodysize);
	}

	void putrequest(Connection *conn, const char* method, const char *url)
	{
		if(conn->m_State != IDLE)
			err_exit("Request already issued");
		
		char req[512];
		
		conn->m_State = REQ_STARTED;
		
		sprintf(req, "%s %s HTTP/1.1", method, url);
		conn->m_Buffer.push_back(req);
		
		//required for HTTP1.1
		putheader(conn, "Host", conn->m_Host.c_str());	

		// don't want any fancy encodings please
		putheader(conn, "Accept-Encoding", "identity");

		// Push a new response onto the queue
		Response *r = new Response;
		response_init(r, method, conn);
		conn->m_Outstanding.push_back(r);
	}
	
	void putheader(Connection *conn, const char* header, const char* value)
	{
		if (conn->m_State != REQ_STARTED)
			err_exit("putheader() failed");
		conn->m_Buffer.push_back(string(header) + ": " + value);
	}

	void putheader(Connection *conn, const char* header, int numericvalue)
	{
		char buf[32];
		sprintf(buf, "%d", numericvalue);
		putheader(conn, header, buf);
	}

	void endheaders(Connection *conn)
	{
		if (conn->m_State != REQ_STARTED)
			err_exit("Cannot send header");
		conn->m_State = IDLE;

		conn->m_Buffer.push_back("");

		string msg;
		for (auto it = conn->m_Buffer.begin(); it != conn->m_Buffer.end(); ++it)
			msg += (*it) + "\r\n";

		conn->m_Buffer.clear();
		send(conn, msg.c_str(), msg.size());
	}

	void send(Connection *conn, const char* buf, int buflen)
	{
		ssize_t nsent;
		if (conn->m_Sock < 0)
			tcp_connect(conn);

		while (buflen > 0) {
			nsent = ::send(conn->m_Sock, buf, buflen, 0);
			if(nsent == -1)
				err_exit("send error: %s\n", strerror(errno));
			buflen -= nsent;
			buf += nsent;
		}
	}

//---------------------------------------------------------------------
//
// Response
//
//---------------------------------------------------------------------
	bool completed(Response *resp)
	{
		return resp->m_State == COMPLETE;
	}

	const std::string& get_http_version(const Response *resp)
	{
		return resp->m_VersionString;
	}
	
	// true if connection is expected to close after this response.
	bool willclose(Response *resp)
	{
		return resp->m_WillClose;
	}

	void response_init(Response *resp, const char *method, Connection *conn)
	{
		resp->m_Connection = conn;
		resp->m_State = STATUSLINE;
		resp->m_Method = method;
		resp->m_Version = 0;
		resp->m_Status = 0;
		resp->m_BytesRead = 0;
		resp->m_Chunked = false;
		resp->m_ChunkLeft = 0;
		resp->m_Length = -1;
		resp->m_WillClose = false;
	}

	const char* getheader(const Response *resp, const char* name)
	{
		std::string lname(name);
		size_t i;

		for (i = 0; i < lname.size(); i++)
			lname[i] = tolower(lname[i]);

		auto it = resp->m_Headers.find(lname);
		if (it == resp->m_Headers.end())
			return NULL;
		else
			return it->second.c_str();
	}

	int getstatus(const Response *resp) 
	{
		// only valid once we've got the statusline
		assert(resp->m_State != STATUSLINE);
		return resp->m_Status;
	}

	const char* getreason(const Response *resp) 
	{
		// only valid once we've got the statusline
		assert(resp->m_State != STATUSLINE);
		return resp->m_Reason.c_str();
	}

// Connection has closed
	void notifyconnectionclosed(Response *resp)
	{
		if (resp->m_State == COMPLETE)
			return;

		// eof can be valid...
		if (resp->m_State == BODY && !resp->m_Chunked && resp->m_Length == -1)
			Finish(resp);	// we're all done!
		else
			err_exit("Connection closed unexpectedly");
	}

	void process_whole_line(Response *resp)
	{
		switch (resp->m_State) {
		case STATUSLINE:
			ProcessStatusLine(resp);
			break;
		case HEADERS:
			ProcessHeaderLine(resp);
			break;
		case TRAILERS:
			ProcessTrailerLine(resp);
			break;
		case CHUNKLEN:
			ProcessChunkLenLine(resp);
			break;
		case CHUNKEND:
			// just soak up the CRLF after body and go to next state
			assert(resp->m_Chunked);
			resp->m_State = CHUNKLEN;
			break;
		default:
			break;
		}
	}
	
	int pump(Response *resp, const unsigned char* data, int datasize)
	{
		assert(datasize != 0);
		int count = datasize;
		char c;
		
		while (count > 0 && resp->m_State != COMPLETE)	{
			if (resp->m_State != BODY) {
				// we want to accumulate a line
				while(count-- > 0) {
					c = *data++;
					if(c == '\n') {
						// now got a whole line!
						process_whole_line(resp);
						resp->m_LineBuf.clear();
						break;	// break out of line accumulation!
					} else {
						if(c != '\r')	// just ignore CR
							resp->m_LineBuf += c;
					}
				}
			} else {
				int bytesused = 0;
				if (resp->m_Chunked)
					bytesused = ProcessDataChunked(resp, data, count);
				else
					bytesused = ProcessDataNonChunked(resp,
									  data, count);
				data += bytesused;
				count -= bytesused;
			}
		}
		// return number of bytes used
		return datasize - count;
	}

	void ProcessChunkLenLine(Response *resp)
	{
		// chunklen in hex at beginning of line
		resp->m_ChunkLeft = strtol(resp->m_LineBuf.c_str(), NULL, 16);
	
		if (resp->m_ChunkLeft == 0) {
			// got the whole body, now check for trailing headers
			resp->m_State = TRAILERS;
			resp->m_HeaderAccum.clear();
		} else {
			resp->m_State = BODY;
		}
	}

// handle some body data in chunked mode
// returns number of bytes used.
	int ProcessDataChunked(Response *resp, const unsigned char* data, int count)
	{
		assert(resp->m_Chunked);

		int n = count;
		if (n > resp->m_ChunkLeft)
			n = resp->m_ChunkLeft;

		// invoke callback to pass out the data
		if (resp->m_Connection->m_ResponseDataCB)
			(resp->m_Connection->m_ResponseDataCB)(resp, resp->m_Connection->m_UserData, data, n );

		resp->m_BytesRead += n;
		resp->m_ChunkLeft -= n;
		assert(resp->m_ChunkLeft >= 0);
		if (resp->m_ChunkLeft == 0) {
			// chunk completed! now soak up the trailing CRLF before next chunk
			resp->m_State = CHUNKEND;
		}
		return n;
	}

// handle some body data in non-chunked mode.
// returns number of bytes used.
	int ProcessDataNonChunked(Response *resp, const unsigned char* data,
				  int count)
	{
		int n = count;
		if (resp->m_Length != -1) {
			// we know how many bytes to expect
			int remaining = resp->m_Length - resp->m_BytesRead;
			if (n > remaining)
				n = remaining;
		}

		// invoke callback to pass out the data
		if(resp->m_Connection->m_ResponseDataCB)
			(resp->m_Connection->m_ResponseDataCB)(resp, resp->m_Connection->m_UserData,
							data, n);

		resp->m_BytesRead += n;

		// Finish if we know we're done. Else we're waiting for connection close.
		if(resp->m_Length != -1 && resp->m_BytesRead == resp->m_Length)
			Finish(resp);
		return n;
	}

	void Finish(Response *resp)
	{
		resp->m_State = COMPLETE;

		if (resp->m_Connection->m_ResponseCompleteCB)
			(resp->m_Connection->m_ResponseCompleteCB) (resp,
							     resp->m_Connection->m_UserData);
	}

	void ProcessStatusLine(Response *resp)
	{
		const char* p = resp->m_LineBuf.c_str();

		// skip any leading space
		while(*p && isspace(*p))
			++p;
		// get version
		while(*p && !isspace(*p))
			resp->m_VersionString += *p++;
		while(*p && isspace(*++p))
			;
		// get status code
		std::string status;
		while(*p && !isspace(*p))
			status += *p++;
		while (*p && isspace(*++p))
			;
		// rest of line is reason
		while (*p)
			resp->m_Reason += *p++;

		resp->m_Status = atoi(status.c_str());

		if(resp->m_Status < 100 || resp->m_Status > 999) /* really happend ?*/
			err_exit("BadStatusLine (%s)", resp->m_LineBuf.c_str()); 

		if (!resp->m_VersionString.compare(0, 8, "HTTP/1.0"))
			resp->m_Version = 10;
		else if(!resp->m_VersionString.compare(0, 8, "HTTP/1.1"))
			resp->m_Version = 11;
		else
			err_exit("UnknownProtocol (%s)", resp->m_VersionString.c_str());
	
		// OK, now we expect headers!
		resp->m_State = HEADERS;
		resp->m_HeaderAccum.clear();
	}

// process accumulated header data
	void FlushHeader(Response *resp)
	{
		if(resp->m_HeaderAccum.empty())
			return;	// no flushing required

		const char* p = resp->m_HeaderAccum.c_str();

		std::string header, value;
		while(*p && *p != ':')
			header += tolower(*p++);

		// skip ':'
		// skip space
		while(*p && isspace(*++p))
			;
		value = p; // rest of line is value
		resp->m_Headers[header] = value;
		printf("%s: %s\n", header.c_str(), value.c_str());

		resp->m_HeaderAccum.clear();
	}

	void ProcessHeaderLine(Response *resp)
	{
		const char* p = resp->m_LineBuf.c_str();
		if (resp->m_LineBuf.empty()) {
			FlushHeader(resp);
			// end of headers

			// HTTP code 100 handling (we ignore 'em)
			if(resp->m_Status == CONTINUE)
				resp->m_State = STATUSLINE;	// reset parsing, expect new status line
			else
				BeginBody(resp);			// start on body now!
			return;
		}

		if (isspace(*p)) {
			// it's a continuation line - just add it to previous data
			while(*p && isspace(*++p))
				;

			resp->m_HeaderAccum += ' ';
			resp->m_HeaderAccum += p;
		} else {
			// begin a new header
			FlushHeader(resp);
			resp->m_HeaderAccum = p;
		}
	}

	void ProcessTrailerLine(Response *resp)
	{
		// TODO: handle trailers?
		// (python httplib doesn't seem to!)
		if (resp->m_LineBuf.empty())
			Finish(resp);
		// just ignore all the trailers...
	}

// OK, we've now got all the headers read in, so we're ready to start
// on the body. But we need to see what info we can glean from the headers
// first...
	void BeginBody(Response *resp)
	{
		resp->m_Chunked = false;
		resp->m_Length = -1;	                // unknown
		resp->m_WillClose = false;

		// using chunked encoding?
		const char* trenc = getheader(resp, "transfer-encoding");
		if (trenc != NULL && !strcasecmp(trenc, "chunked")) {
			resp->m_Chunked = true;
			resp->m_ChunkLeft = -1;	// unknown
		}
		resp->m_WillClose = CheckClose(resp);

		// length supplied?
		const char* contentlen = getheader(resp, "content-length");
		if(contentlen && !resp->m_Chunked)
			resp->m_Length = atoi(contentlen);
	
		// check for various cases where we expect zero-length body
		if (resp->m_Status == NO_CONTENT ||
		    resp->m_Status == NOT_MODIFIED ||
		   (resp->m_Status >= 100 && resp->m_Status < 200) ||
		    resp->m_Method == "HEAD") {
			// 1xx codes have no body
			resp->m_Length = 0;
		}

		// if we're not using chunked mode, and no length has been specified,
		// assume connection will close at end.
		if (!resp->m_WillClose && !resp->m_Chunked && resp->m_Length == -1)
			resp->m_WillClose = true;

		// Invoke the user callback, if any
		if (resp->m_Connection->m_ResponseBeginCB)
			(resp->m_Connection->m_ResponseBeginCB) (resp, resp->m_Connection->m_UserData);
		// now start reading body data!
		if (resp->m_Chunked)
			resp->m_State = CHUNKLEN;
		else
			resp->m_State = BODY;
	}

        // return true if we think server will automatically close connectin at end
	bool CheckClose(Response *resp)
	{
		if(resp->m_Version == 11) {
			// HTTP/1.1
			// the connection stays open unless "connection: close" is specified.
			const char* connection_hdr = getheader(resp, "connection");
			if (connection_hdr && !strcasecmp(connection_hdr, "close"))
				return true;
			else
				return false;
		}

		// Older HTTP
		// keep-alive header indicates persistant connection 
		if (getheader(resp, "keep-alive"))
			return false;

		// TODO: some special case handling for Akamai and netscape maybe?
		// (see _check_close() in python httplib.py for details)
		return true;
	}
}	// end namespace happyhttp

int cnt = 0;
void OnBegin(const happyhttp::Response *r, void* userdata)
{
	printf("%s %d %s\n\n", get_http_version(r).c_str(),
	       getstatus(r), getreason(r));
	cnt = 0;
}

void OnData(const happyhttp::Response* r, void* userdata, const unsigned char* data,
	    int n)
{
	fwrite(data, 1, n, stdout);
	cnt += n;
}

void OnComplete(const happyhttp::Response* r, void* userdata)
{

}

void Test1(const char *host)
{
	// simple simple GET
	struct happyhttp::Connection conn;
	connection_init(&conn, host, 80);
	setcallbacks(&conn, OnBegin, OnData, OnComplete, NULL);

	request(&conn, "GET", "/", 0, 0, 0);

	while (outstanding(&conn))
		pump(&conn);
	connection_destroy(&conn);
}

void Test2()
{
	printf("-----------------Test2------------------------\n" );
	// POST using high-level request interface

	const char* headers[] = {
		"Connection", "close",
		"Content-type", "application/x-www-form-urlencoded",
		"Accept", "text/plain",
		NULL
	};

	const char* body = "answer=42&name=Bubba";
	struct happyhttp::Connection conn;
	
	connection_init(&conn, "www.scumways.com", 80);
	setcallbacks(&conn, OnBegin, OnData, OnComplete, 0);
	request(&conn, "POST", "/happyhttp/test.php", headers,
		     (const unsigned char*) body, strlen(body));

	while(outstanding(&conn))
		pump(&conn);
	connection_destroy(&conn);
}

void Test3()
{
	printf("-----------------Test3------------------------\n" );
	// POST example using lower-level interface

	const char* params = "answer=42&foo=bar";
	int len = strlen(params);

	happyhttp::Connection conn;
	connection_init(&conn, "www.scumways.com", 80);
	setcallbacks(&conn, OnBegin, OnData, OnComplete, 0);

	putrequest(&conn, "POST", "/happyhttp/test.php");
	putheader(&conn, "Connection", "close");
	putheader(&conn, "Content-Length", len);
	putheader(&conn, "Content-type", "application/x-www-form-urlencoded" );
	putheader(&conn, "Accept", "text/plain");
	endheaders(&conn);
	send(&conn, params, len);

	while (outstanding(&conn))
		pump(&conn);
}

int main(int argc, char *argv[])
{
	if (argc != 2)
		happyhttp::err_exit("Usage: a.out <host>\n");
	Test1(argv[1]);
	return 0;
}
