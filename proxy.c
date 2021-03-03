/****************************************************************************
 * proxy.c finished by Sizhe Li, 1900013061                                 *
 *                                                                          *
 * In this file, a proxy between client and server is implemented.          *
 *                                                                          *
 * We receive requests from the client and send it to the server.           *
 * Then we accept the response from the server and send back to the client. *
 *                                                                          *
 * We use multi-thread to achieve concurrent programming.                   *
 * And we add a cache to the proxy so that when the uri hits, it can        *
 * directly send back the saved response without connecting to the server.  *
 *                                                                          *
 ****************************************************************************/

#include "csapp.h"
#include <stdio.h>

#define MAX_CACHE_SIZE 1024000  /* The max cache size */
#define MAX_OBJECT_SIZE 102500  /* The max size of each cache line */
#define COUNT 10    /* Total num of cache lines */

const char* user_agent_header = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
const char* conn_header = "Connection: close\r\n";
const char* prox_header = "Proxy-Connection: close\r\n";

/* Define the cache struct */
typedef struct
{
	char cache_word[MAX_OBJECT_SIZE];   /* to save the response line */
	char cache_url[MAXLINE];    /* to save the uri */
	int timetag;    /* to mark the time when it is read or write */
	int empty;  /* if this line is empty */
	int readcnt;    /* record the number of readers */
	sem_t mutex_wt;
	sem_t mutex_rdcnt;
	sem_t mutex_time;
}cache_line;
cache_line cache[COUNT];

/* functions about cache */
void cache_init();
int cache_find(char* url);
int cache_evict();
void cache_cache(char* uri, char* buf);

/* functions to support the proxy */
void* thread(void* vargp);
void doit(int fd);
void clienterror(int fd, char* cause, char* errnum,
	char* shortmsg, char* longmsg);
void parse_uri(char* uri, char* hostname, char* filename, int* port);
void build_header(rio_t* rtmp, char* reqhdr, char* hostname, 
                                        char* filename, char* port);

/*  Function : cache_init
 *  Initialize the Cache */
void cache_init()
{
	for (int i = 0; i < COUNT; i++) 
    {
		cache[i].timetag = 0;
		cache[i].empty = 1;
		Sem_init(&cache[i].mutex_wt, 0, 1);
		Sem_init(&cache[i].mutex_rdcnt, 0, 1);
		Sem_init(&cache[i].mutex_time, 0, 1);
		cache[i].readcnt = 0;
	}
}

/*  Function : cache_find
 *  find if the url already exists in the cache, if so, return the index.
 *  Otherwise, return -1.
 *  Before reading, just lock the semaphore */
int cache_find(char* url)
{
	for (int i = 0; i < COUNT; i++)
	{
        /* lock to protect the rdcnt */
		P(&cache[i].mutex_rdcnt);
		cache[i].readcnt++;
		if (cache[i].readcnt == 1)
			P(&cache[i].mutex_wt);
		V(&cache[i].mutex_rdcnt);

        /* We find the url in the cache */
		if ((!cache[i].empty) && (!strcmp(url, cache[i].cache_url)))
		{
			P(&cache[i].mutex_rdcnt);
			cache[i].readcnt--;
			if (cache[i].readcnt == 0)
				V(&cache[i].mutex_wt);
			V(&cache[i].mutex_rdcnt);
			return i;
		}

		P(&cache[i].mutex_rdcnt);
		cache[i].readcnt--;
		if (cache[i].readcnt == 0)
			V(&cache[i].mutex_wt);
		V(&cache[i].mutex_rdcnt);
	}
	return -1;
}

/*  Function : cache_evict
 *  To find which line should be evicted.
 *  If we find an empty line, just fill it.
 *  If the cache is full, we use lru method to find the
 *  least recently used line. */
int cache_evict() 
{
	int min = COUNT, index = 0;

	for (int i = 0; i < COUNT; i++)
	{
        /* lock to protect the rdcnt */
		P(&cache[i].mutex_rdcnt);
		cache[i].readcnt++;
		if (cache[i].readcnt == 1)
			P(&cache[i].mutex_wt);
		V(&cache[i].mutex_rdcnt);

        /* Find an empty line and just return it */
		if (cache[i].empty == 1)
		{
			P(&cache[i].mutex_rdcnt);
			cache[i].readcnt--;
			if (cache[i].readcnt == 0)
				V(&cache[i].mutex_wt);
			V(&cache[i].mutex_rdcnt);

			return i;
		}

        /* record the least recently used line up to now */
		if (cache[i].timetag < min)
		{
			min = cache[i].timetag;
			index = i;
		}

		P(&cache[i].mutex_rdcnt);
		cache[i].readcnt--;
		if (cache[i].readcnt == 0)
			V(&cache[i].mutex_wt);
		V(&cache[i].mutex_rdcnt);
	}
	return index;
}

/*  Function : cache_cache
 *  to save the uri and the request in the cache.
 *  Firstly we use cache_eviction function to find which line we can use.
 *  Then we save the contents and the uri.
 *  In the end, we should update the timetag of other lines. */
void cache_cache(char* uri, char* buf) 
{
	int evict = cache_evict();
    /* It is the only writer */
	P(&cache[evict].mutex_wt);

    /* Save the uri and requests */
	memcpy(cache[evict].cache_word, buf, MAX_OBJECT_SIZE);
	strcpy(cache[evict].cache_url, uri);
	cache[evict].empty = 0;
	cache[evict].timetag = COUNT;

    /* Update the others' timetag */
	for (int i = 0; i < COUNT; i++)
	{
		if (i == evict)
			continue;
		P(&cache[i].mutex_time);
		if (!cache[i].empty)
			cache[i].timetag--;
		V(&cache[i].mutex_time);
	}

	V(&cache[evict].mutex_wt);
}

/*  Function : main(just like the main function in tiny)
 *  The main function of the proxy.
 *  Keep running and receiving connection requests, 
 *  create threads to deal with the connection */
int main(int argc, char** argv)
{
	signal(SIGPIPE, SIG_IGN);

	int listenfd, * connfd;
	pthread_t tid;
	char hostname[MAXLINE], port[MAXLINE];
	socklen_t clientlen;
	struct sockaddr_storage clientaddr;

	/* Check command line args */
	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}

	cache_init();

	listenfd = Open_listenfd(argv[1]);
	while (1)
	{
		clientlen = sizeof(clientaddr);
		connfd = Malloc(sizeof(int));
		*connfd = Accept(listenfd, (SA*)&clientaddr, &clientlen); //line:netp:tiny:accept
		Getnameinfo((SA*)&clientaddr, clientlen, hostname, MAXLINE,
			port, MAXLINE, 0);
		//       printf("Accepted connection from (%s, %s)\n", hostname, port);
		Pthread_create(&tid, NULL, thread, connfd);                                          //line:netp:tiny:close
	}
    Close(listenfd);
}

/*  Function : thread
 *  The new thread deals with the connection request. */
void* thread(void* vargp)
{
	int connfd = *((int*)vargp);
	Pthread_detach(pthread_self());
	Free(vargp);
	doit(connfd);
	Close(connfd);
	return NULL;
}

/*  Function : doit
 *  Firstly, we read the request line and headers.
 *  Then, we roughly judge whether the request is legal or not.
 *  Next we parse the request line and search in the cache.
 *  If cache hits, we directly send the response to the client.
 *  Else, we send the response to the server and cache the response.*/
void doit(int fd_client)
{
	int port, fd_server;
	char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
	char hostname[MAXLINE], filename[MAXLINE];
	rio_t rio_client, rio_server;

	/* Read request line and headers */
	Rio_readinitb(&rio_client, fd_client);

	if (!Rio_readlineb(&rio_client, buf, MAXLINE))
		return;

	sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET") || strstr(uri, "https")) 
    {
		clienterror(fd_client, method, "501", "Not Implemented",
			"Tiny does not implement this method");
		return;
	}

    parse_uri(uri, hostname, filename, &port);
    char c_uri[MAXLINE]; /* the uri to be saved in cache(hostname+filename) */
    strcpy(c_uri, hostname);
    strcat(c_uri, filename);

	int index;
	if ((index = cache_find(c_uri)) != -1) /* cache hits */
	{
		P(&cache[index].mutex_rdcnt);
		cache[index].readcnt++;
		if (cache[index].readcnt == 1)
			P(&cache[index].mutex_wt);
		V(&cache[index].mutex_rdcnt);

        /* Directly send the response to the client */
		Rio_writen(fd_client, cache[index].cache_word, MAX_OBJECT_SIZE);

		P(&cache[index].mutex_rdcnt);
		cache[index].readcnt--;
		if (cache[index].readcnt == 0)
			V(&cache[index].mutex_wt);
		V(&cache[index].mutex_rdcnt);
		return;
	}
	
    /* get ready to connect to the server */
	char port_str[10];
	sprintf(port_str, "%d", port);

	fd_server = Open_clientfd(hostname, port_str);
	if (fd_server < 0)
	{
		clienterror(fd_server, method, "404", "Not Implemented",
			"Tiny does not implement this method");
		return;
	}

    /* Send the request header to the server */
	char reqhdr[MAXLINE];
	build_header(&rio_client, reqhdr, hostname, filename, port_str);
	Rio_writen(fd_server, reqhdr, strlen(reqhdr));

    /* receive the response and save in the cache */
	char cachebuf[MAX_OBJECT_SIZE];
	int n, sizebuf = 0;
    Rio_readinitb(&rio_server, fd_server);
	while ((n = Rio_readlineb(&rio_server, buf, MAXLINE)))
	{
		if (sizebuf + n <= MAX_OBJECT_SIZE)
        {
            memcpy(cachebuf + sizebuf, buf, n);
            sizebuf += n;
        }	
		Rio_writen(fd_client, buf, n);
	}

    /* If the response line is too long, the abandon it */
	if (sizebuf <= MAX_OBJECT_SIZE)
		cache_cache(c_uri, cachebuf);
    Close(fd_server);
}

/*  Function : parse_uri
 *  The default port number is 80.
 *  If there is a port num, then ':' must exist.
 *  We can go to the port number through this and then easily to parse the 
 *  hostname and the filename.
 *  Thecharacter before the filename must be either '/' or port num.
 *  So we can go to the filename by this way. */
void parse_uri(char* uri, char* hostname, char* filename, int* port)
{
	*port = 80;
    /* if the uri contains 'http//' */
	char* pos = strstr(uri, "//");
	if (pos != NULL)
		uri = pos + 2;

    /* if the uri only contains a hostname and a filename */
    pos = strstr(uri, "/"); 
    if(pos != NULL)
    {
        *pos = '\0';
		strcpy(hostname, uri);
		*pos = '/';
		strcpy(filename, pos);
    }

	pos = strstr(uri, ":");
	if(pos == NULL) /* there is not a port num */
	{
		strcpy(hostname, uri);
		strcpy(filename, "");
	}
    else /* port num exists */
    {
		*pos = '\0';
		strcpy(hostname, uri);
		sscanf(pos + 1, "%d%s", port, filename);
		*pos = ':';
	}
}

/*  Function : build_header
 *  Build the request header    */
void build_header(rio_t* rtmp, char* reqhdr, char* hostname,
    char* filename, char* port)
{
	char buf[MAXLINE];
    sprintf(reqhdr, "GET %s HTTP/1.0\r\n", filename);

	while (Rio_readlineb(rtmp, buf, MAXLINE) > 0)
	{
		if (!strcmp(buf, "\r\n"))
			break;
		if (strstr(buf, "Host:") != NULL)
			continue;
		if (strstr(buf, "User-Agent:") != NULL)
			continue;
		if (strstr(buf, "Connection:") != NULL)
			continue;
		if (strstr(buf, "Proxy-Connection:") != NULL)
			continue;

		sprintf(reqhdr, "%s%s", reqhdr, buf);
	}
	sprintf(reqhdr, "%sHost: %s:%s\r\n", reqhdr, hostname, port);
	sprintf(reqhdr, "%s%s%s%s\r\n", reqhdr, user_agent_header,
             conn_header, prox_header);
}

void clienterror(int fd, char* cause, char* errnum,
	char* shortmsg, char* longmsg)
{
	char buf[MAXLINE], body[MAXBUF];

	/* Build the HTTP response body */
	sprintf(body, "<html><title>Tiny Error</title>");
	sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
	sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
	sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
	sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

	/* Print the HTTP response */
	sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
	Rio_writen(fd, buf, strlen(buf));
	sprintf(buf, "Content-type: text/html\r\n");
	Rio_writen(fd, buf, strlen(buf));
	sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
	Rio_writen(fd, buf, strlen(buf));
	Rio_writen(fd, body, strlen(body));
}