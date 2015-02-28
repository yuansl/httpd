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
	Connection::Connection(const char* host, int port) :
		m_ResponseBeginCB(NULL),
		m_ResponseDataCB(NULL),
		m_ResponseCompleteCB(NULL),
		m_UserData(0),
		m_State(IDLE),
		m_Host(host),
		m_Port(port),
		m_Sock(-1)
	{
	}

	void Connection::setcallbacks(ResponseBegin_CB begincb,
				      ResponseData_CB datacb,
				      ResponseComplete_CB completecb, void *userdata)
	{
		m_ResponseBeginCB = begincb;
		m_ResponseDataCB = datacb;
		m_ResponseCompleteCB = completecb;
		m_UserData = userdata;
	}

	void Connection::connect()
	{
		struct addrinfo hints, *res, *pres;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = 0;

		if (getaddrinfo(m_Host.c_str(), "http", &hints, &res) != 0) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(h_errno));
			exit(EXIT_FAILURE);
		}
		
		for (pres = res; pres; pres = pres->ai_next) {
			m_Sock = socket(pres->ai_family, pres->ai_socktype,
				pres->ai_protocol);
			if (m_Sock == -1)
				continue;
			if (::connect(m_Sock, pres->ai_addr, pres->ai_addrlen) == 0)
				break;
			::close(m_Sock);
		}
		if (pres == NULL) {
			fprintf(stderr, "connect error: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		freeaddrinfo(res);
	}

	void Connection::close()
	{
		m_Sock = -1;
		// discard any incomplete responses
		while (!m_Outstanding.empty()) {
			delete m_Outstanding.front();
			m_Outstanding.pop_front();
		}
	}

	Connection::~Connection()
	{
		close();
	}

	void Connection::request(const char *method, const char *url,
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

		putrequest(method, url);
		if (body != NULL && !gotcontentlength)
			putheader("Content-Length", bodysize);
		if (headers) {
			const char **h = headers;
			while (*h) {
				const char *name = *h++;
				const char *value = *h++;
				putheader(name, value);
			}
		}
		endheaders();
		if (body)
			send((const char *) body, bodysize);
	}

	void Connection::putrequest(const char* method, const char *url)
	{
		if(m_State != IDLE)
			err_exit("Request already issued");
		
		char req[512];
		
		m_State = REQ_STARTED;
		
		sprintf(req, "%s %s HTTP/1.1", method, url);
		m_Buffer.push_back(req);
		
		//required for HTTP1.1
		putheader("Host", m_Host.c_str());	

		// don't want any fancy encodings please
		putheader("Accept-Encoding", "identity");

		// Push a new response onto the queue
		Response *r = new Response(method, *this);
		m_Outstanding.push_back(r);
	}
	
	void Connection::putheader(const char* header, const char* value)
	{
		if (m_State != REQ_STARTED)
			err_exit("putheader() failed");
		m_Buffer.push_back(string(header) + ": " + value);
	}

	void Connection::putheader(const char* header, int numericvalue)
	{
		char buf[32];
		sprintf(buf, "%d", numericvalue);
		putheader(header, buf);
	}

	void Connection::endheaders()
	{
		if(m_State != REQ_STARTED)
			err_exit("Cannot send header");
		m_State = IDLE;

		m_Buffer.push_back("");

		string msg;
		for (auto it = m_Buffer.begin(); it != m_Buffer.end(); ++it)
			msg += (*it) + "\r\n";

		m_Buffer.clear();
		send(msg.c_str(), msg.size());
	}

	void Connection::send(const char* buf, int buflen)
	{
		ssize_t nsent;
		if(m_Sock < 0)
			connect();

		while (buflen > 0) {
			nsent = ::send(m_Sock, buf, buflen, 0);
			if(nsent == -1)
				err_exit("send error: %s\n", strerror(errno));
			buflen -= nsent;
			buf += nsent;
		}
	}

	void Connection::pump()
	{
		if (m_Outstanding.empty())
			return;		// no requests outstanding

		Response *r;
		assert(m_Sock > 0);	// outstanding requests but no connection!

		if (!datawaiting(m_Sock))
			return;				// recv will block

		unsigned char buf[2048];
		ssize_t n = recv(m_Sock, buf, sizeof(buf), 0);

		if (n < 0)
			err_exit("recv error: %s\n", strerror(errno));

		if (n == 0) {
			// connection has closed
			r = m_Outstanding.front();
			r->notifyconnectionclosed();
			assert(r->completed());
			delete r;
			m_Outstanding.pop_front();

			// any outstanding requests will be discarded
			close();
		} else {
			int used = 0;
			while (used < n && !m_Outstanding.empty()) {
				r = m_Outstanding.front();
				int u = r->pump(buf + used, n - used);

				// delete response once completed
				if (r->completed()) {
					delete r;
					m_Outstanding.pop_front();
				}
				used += u;
			}

			// NOTE: will lose bytes if response queue goes empty
			// (but server shouldn't be sending anything if we don't have
			// anything outstanding anyway)
			assert(used == n);	// all bytes should be used up by here.
		}
	}

//---------------------------------------------------------------------
//
// Response
//
//---------------------------------------------------------------------

	Response::Response(const char* method, Connection& conn) : m_Connection(conn),
								   m_State(STATUSLINE),
								   m_Method(method),
								   m_Version(0),
								   m_Status(0),
								   m_BytesRead(0),
								   m_Chunked(false),
								   m_ChunkLeft(0),
								   m_Length(-1),
								   m_WillClose(false)
	{
	}

	const char* Response::getheader(const char* name) const
	{
		std::string lname(name);
		size_t i;

		for (i = 0; i < lname.size(); i++)
			lname[i] = tolower(lname[i]);

		auto it = m_Headers.find(lname);
		if(it == m_Headers.end())
			return NULL;
		else
			return it->second.c_str();
	}

	int Response::getstatus() const
	{
		// only valid once we've got the statusline
		assert(m_State != STATUSLINE);
		return m_Status;
	}

	const char* Response::getreason() const
	{
		// only valid once we've got the statusline
		assert(m_State != STATUSLINE);
		return m_Reason.c_str();
	}

// Connection has closed
	void Response::notifyconnectionclosed()
	{
		if(m_State == COMPLETE)
			return;

		// eof can be valid...
		if(m_State == BODY && !m_Chunked && m_Length == -1)
			Finish();	// we're all done!
		else
			err_exit("Connection closed unexpectedly");
	}

	void Response::process_whole_line()
	{
		switch (m_State) {
		case STATUSLINE:
			ProcessStatusLine();
			break;
		case HEADERS:
			ProcessHeaderLine();
			break;
		case TRAILERS:
			ProcessTrailerLine();
			break;
		case CHUNKLEN:
			ProcessChunkLenLine();
			break;
		case CHUNKEND:
			// just soak up the CRLF after body and go to next state
			assert(m_Chunked);
			m_State = CHUNKLEN;
			break;
		default:
			break;
		}
	}
	
	int Response::pump(const unsigned char* data, int datasize)
	{
		assert(datasize != 0);
		int count = datasize;
		char c;
		
		while(count > 0 && m_State != COMPLETE)	{
			if (m_State != BODY) {
				// we want to accumulate a line
				while(count-- > 0) {
					c = *data++;
					if(c == '\n') {
						// now got a whole line!
						process_whole_line();
						m_LineBuf.clear();
						break;	// break out of line accumulation!
					} else {
						if(c != '\r')	// just ignore CR
							m_LineBuf += c;
					}
				}
			} else {
				int bytesused = 0;
				if (m_Chunked)
					bytesused = ProcessDataChunked(data, count);
				else
					bytesused = ProcessDataNonChunked(data,
									  count);
				data += bytesused;
				count -= bytesused;
			}
		}
		// return number of bytes used
		return datasize - count;
	}

	void Response::ProcessChunkLenLine()
	{
		// chunklen in hex at beginning of line
		m_ChunkLeft = strtol(m_LineBuf.c_str(), NULL, 16);
	
		if (m_ChunkLeft == 0) {
			// got the whole body, now check for trailing headers
			m_State = TRAILERS;
			m_HeaderAccum.clear();
		} else {
			m_State = BODY;
		}
	}

// handle some body data in chunked mode
// returns number of bytes used.
	int Response::ProcessDataChunked(const unsigned char* data, int count)
	{
		assert(m_Chunked);

		int n = count;
		if (n > m_ChunkLeft)
			n = m_ChunkLeft;

		// invoke callback to pass out the data
		if (m_Connection.m_ResponseDataCB)
			(m_Connection.m_ResponseDataCB)(this, m_Connection.m_UserData, data, n );

		m_BytesRead += n;
		m_ChunkLeft -= n;
		assert(m_ChunkLeft >= 0);
		if(m_ChunkLeft == 0) {
			// chunk completed! now soak up the trailing CRLF before next chunk
			m_State = CHUNKEND;
		}
		return n;
	}

// handle some body data in non-chunked mode.
// returns number of bytes used.
	int Response::ProcessDataNonChunked(const unsigned char* data, int count)
	{
		int n = count;
		if (m_Length != -1) {
			// we know how many bytes to expect
			int remaining = m_Length - m_BytesRead;
			if (n > remaining)
				n = remaining;
		}

		// invoke callback to pass out the data
		if(m_Connection.m_ResponseDataCB)
			(m_Connection.m_ResponseDataCB)(this, m_Connection.m_UserData,
							data, n);

		m_BytesRead += n;

		// Finish if we know we're done. Else we're waiting for connection close.
		if(m_Length != -1 && m_BytesRead == m_Length)
			Finish();
		return n;
	}

	void Response::Finish()
	{
		m_State = COMPLETE;

		if (m_Connection.m_ResponseCompleteCB)
			(m_Connection.m_ResponseCompleteCB) (this,
							     m_Connection.m_UserData);
	}

	void Response::ProcessStatusLine()
	{
		const char* p = m_LineBuf.c_str();

		// skip any leading space
		while(*p && isspace(*p))
			++p;
		// get version
		while(*p && !isspace(*p))
			m_VersionString += *p++;
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
			m_Reason += *p++;

		m_Status = atoi(status.c_str());

		if(m_Status < 100 || m_Status > 999) /* really happend ?*/
			err_exit("BadStatusLine (%s)", m_LineBuf.c_str()); 

		if (!m_VersionString.compare(0, 8, "HTTP/1.0"))
			m_Version = 10;
		else if(!m_VersionString.compare(0, 8, "HTTP/1.1"))
			m_Version = 11;
		else
			err_exit("UnknownProtocol (%s)", m_VersionString.c_str());
	
		// OK, now we expect headers!
		m_State = HEADERS;
		m_HeaderAccum.clear();
	}

// process accumulated header data
	void Response::FlushHeader()
	{
		if(m_HeaderAccum.empty())
			return;	// no flushing required

		const char* p = m_HeaderAccum.c_str();

		std::string header, value;
		while(*p && *p != ':')
			header += tolower(*p++);

		// skip ':'
		// skip space
		while(*p && isspace(*++p))
			;
		value = p; // rest of line is value
		m_Headers[header] = value;
		printf("%s: %s\n", header.c_str(), value.c_str());

		m_HeaderAccum.clear();
	}

	void Response::ProcessHeaderLine()
	{
		const char* p = m_LineBuf.c_str();
		if (m_LineBuf.empty()) {
			FlushHeader();
			// end of headers

			// HTTP code 100 handling (we ignore 'em)
			if(m_Status == CONTINUE)
				m_State = STATUSLINE;	// reset parsing, expect new status line
			else
				BeginBody();			// start on body now!
			return;
		}

		if (isspace(*p)) {
			// it's a continuation line - just add it to previous data
			while(*p && isspace(*++p))
				;

			m_HeaderAccum += ' ';
			m_HeaderAccum += p;
		} else {
			// begin a new header
			FlushHeader();
			m_HeaderAccum = p;
		}
	}

	void Response::ProcessTrailerLine()
	{
		// TODO: handle trailers?
		// (python httplib doesn't seem to!)
		if (m_LineBuf.empty())
			Finish();
		// just ignore all the trailers...
	}

// OK, we've now got all the headers read in, so we're ready to start
// on the body. But we need to see what info we can glean from the headers
// first...
	void Response::BeginBody()
	{
		m_Chunked = false;
		m_Length = -1;	                // unknown
		m_WillClose = false;

		// using chunked encoding?
		const char* trenc = getheader("transfer-encoding");
		if (trenc && 0 == strcasecmp(trenc, "chunked")) {
			m_Chunked = true;
			m_ChunkLeft = -1;	// unknown
		}
		m_WillClose = CheckClose();

		// length supplied?
		const char* contentlen = getheader("content-length");
		if(contentlen && !m_Chunked)
			m_Length = atoi(contentlen);
	
		// check for various cases where we expect zero-length body
		if(m_Status == NO_CONTENT || m_Status == NOT_MODIFIED ||
		   (m_Status >= 100 && m_Status < 200) || m_Method == "HEAD")
		{ // 1xx codes have no body
			m_Length = 0;
		}

		// if we're not using chunked mode, and no length has been specified,
		// assume connection will close at end.
		if(!m_WillClose && !m_Chunked && m_Length == -1)
			m_WillClose = true;

		// Invoke the user callback, if any
		if(m_Connection.m_ResponseBeginCB)
			(m_Connection.m_ResponseBeginCB) (this, m_Connection.m_UserData);
		std::cout << '\n';

		// now start reading body data!
		if(m_Chunked)
			m_State = CHUNKLEN;
		else
			m_State = BODY;
	}

// return true if we think server will automatically close connection
	bool Response::CheckClose()
	{
		if(m_Version == 11) {
			// HTTP/1.1
			// the connection stays open unless "connection: close" is specified.
			const char* connection_hdr = getheader("connection");
			if (connection_hdr && !strcasecmp(connection_hdr, "close"))
				return true;
			else
				return false;
		}

		// Older HTTP
		// keep-alive header indicates persistant connection 
		if (getheader("keep-alive"))
			return false;

		// TODO: some special case handling for Akamai and netscape maybe?
		// (see _check_close() in python httplib.py for details)
		return true;
	}
}	// end namespace happyhttp

int cnt = 0;
void OnBegin(const happyhttp::Response *r, void* userdata)
{
	printf("%s %d %s\n", r->get_http_version().c_str(),
	       r->getstatus(), r->getreason());
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
	happyhttp::Connection conn(host, 80);
	conn.setcallbacks(OnBegin, OnData, OnComplete, NULL);

	conn.request("GET", "/", 0, 0, 0);

	while(conn.outstanding())
		conn.pump();
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
	happyhttp::Connection conn("www.scumways.com", 80);
	conn.setcallbacks(OnBegin, OnData, OnComplete, 0);
	conn.request("POST", "/happyhttp/test.php", headers,
		     (const unsigned char*) body, strlen(body));

	while(conn.outstanding())
		conn.pump();
}

void Test3()
{
	printf("-----------------Test3------------------------\n" );
	// POST example using lower-level interface

	const char* params = "answer=42&foo=bar";
	int len = strlen(params);

	happyhttp::Connection conn("www.scumways.com", 80);
	conn.setcallbacks(OnBegin, OnData, OnComplete, 0);

	conn.putrequest("POST", "/happyhttp/test.php");
	conn.putheader("Connection", "close");
	conn.putheader("Content-Length", len);
	conn.putheader("Content-type", "application/x-www-form-urlencoded" );
	conn.putheader("Accept", "text/plain");
	conn.endheaders();
	conn.send(params, len);

	while (conn.outstanding())
		conn.pump();
}

int main(int argc, char *argv[])
{
	if (argc != 2)
		happyhttp::err_exit("Usage: a.out <host>\n");
	Test1(argv[1]);
	Test3();
	return 0;
}
