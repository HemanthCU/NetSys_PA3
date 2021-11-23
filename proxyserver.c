/* 
 * proxyserver.c - A server to handle different HTTP requests to load a website
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>      /* for fgets */
#include <strings.h>     /* for bzero, bcopy */
#include <unistd.h>      /* for read, write */
#include <sys/socket.h>  /* for socket use */
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <openssl/md5.h>

#define MAXLINE  8192  /* max text line length */
#define MAXBUF   8192  /* max I/O buffer size */
#define LISTENQ  1024  /* second argument to listen() */
#define MAXREAD  80000
#define HOSTLEN  100

int open_listenfd(int port);
int open_sendfd(int port, char *host);
void echo(int connfd);
void *thread(void *vargp);
char* getContentType(char *tgtpath);
char* hostname_to_ip(char *hostname);
char* checkCache(char *url);
char* str2md5(const char *str, int length);

char ***cacheList;
int cacheLen;

int main(int argc, char **argv) {
    int i, j, listenfd, *connfdp, port, clientlen=sizeof(struct sockaddr_in);
    cacheList = (char***) malloc (400 * sizeof(char**));
    for (i = 0; i < 400; i++) {
        cacheList[i] = (char**) malloc (3 * sizeof(char*));
        for (j = 0; j < 3; j++) {
            cacheList[i][j] = (char*) malloc (HOSTLEN * sizeof(char));
            bzero(cacheList[i][j], HOSTLEN);
        }
    }
    cacheLen = 0;
    struct sockaddr_in clientaddr;
    pthread_t tid; 

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }
    port = atoi(argv[1]);

    listenfd = open_listenfd(port);
    while (1) {
        connfdp = malloc(sizeof(int));
        *connfdp = accept(listenfd, (struct sockaddr*)&clientaddr, &clientlen);
        // Create a new thread only if a new connection arrives
        if (*connfdp >= 0)
            pthread_create(&tid, NULL, thread, connfdp);
    }
}

/* 
 * getContentType - gets the content type that needs to be added to HTTP response
 * Returns NULL in case of failure 
 */
char* getContentType(char *tgtpath) {
    char *temp1 = (char*) malloc (100*sizeof(char));
    char *temp2 = (char*) malloc (100*sizeof(char));
    char *temp3 = NULL;
    char *temp4 = (char*) malloc (100*sizeof(char));
    strcpy(temp1, tgtpath);
    temp3 = strrchr(temp1, '.');
    if (temp3 == NULL) {
        printf("ERROR in file type\n");
        return NULL;
    }
    temp2 = temp3 + 1;
    if (strcmp(temp2, "html") == 0) {
        strcpy(temp4, "text/html");
    } else if (strcmp(temp2, "txt") == 0) {
        strcpy(temp4, "text/plain");
    } else if (strcmp(temp2, "png") == 0) {
        strcpy(temp4, "image/png");
    } else if (strcmp(temp2, "gif") == 0) {
        strcpy(temp4, "image/gif");
    } else if (strcmp(temp2, "jpg") == 0) {
        strcpy(temp4, "image/jpg");
    } else if (strcmp(temp2, "ico") == 0) {
        strcpy(temp4, "image/x-icon");
    } else if (strcmp(temp2, "css") == 0) {
        strcpy(temp4, "text/css");
    } else if (strcmp(temp2, "js") == 0) {
        strcpy(temp4, "application/javascript");
    } else {
        temp4 = NULL;
    }

    return temp4;
}

char* checkCache(char *url) {
    int i;
    for (i = 0; i < cacheLen; i++) {
        if(strcmp(url, cacheList[i][0]) == 0) {
            return cacheList[i][1];
        }
    }
    return "";
}

char* str2md5(const char *str, int length) {
    int n;
    MD5_CTX c;
    unsigned char digest[16];
    char *out = (char*) malloc (33);

    MD5_Init(&c);

    while (length > 0) {
        if (length > 512) {
            MD5_Update(&c, str, 512);
        } else {
            MD5_Update(&c, str, length);
        }
        length -= 512;
        str += 512;
    }

    MD5_Final(digest, &c);

    for (n = 0; n < 16; n++) {
        snprintf(&(out[n*2]), 16*2, "%02x", (unsigned int)digest[n]);
    }

    return out;
}

/* thread routine */
void * thread(void * vargp) {
    int connfd = *((int *)vargp);
    int sendfd;
    pthread_detach(pthread_self());
    free(vargp);
    // Process the header to get details of request
    size_t n, m;
    int i, found;
    int keepalive = 0; /* Denotes if the current request is persistent */
    int first = 1; /* Denotes if this is the first execution of the while loop */
    int msgsz; /* Size of data read from the file */
    char buf[MAXLINE]; /* Request full message */
    char buf1[MAXLINE]; /* Request full message */
    char *resp = (char*) malloc (MAXBUF*sizeof(char)); /* Response header */
    unsigned char *msg = (char*) malloc (MAXREAD*sizeof(char)); /* Data read from the file */
    char *context = NULL; /* Pointer used for string tokenizer */
    char *comd; /* Incoming HTTP command */
    char *host; /* Incoming HTTP host */
    char *temp = NULL; /* Temporary pointer to check if connection is persistent */
    char *tgtpath; /* Incoming HTTP URL */
    char *tgtpath1 = (char*) malloc (100*sizeof(char)); /* Path to the file referenced by URL */
    char *httpver; /* Incoming HTTP version */
    char *contType; /* Content type of the data being returned */
    char *postdata; /* Postdata to be appended to the request */
    char *url = (char*) malloc (100*sizeof(char));
    char *fpath;
    char c;
    FILE *fp; /* File descriptor to open the file */
    while (keepalive || first) {
        if (!first)
            printf("Waiting for incoming request\n");
        else
            first = 0;
        n = read(connfd, buf, MAXLINE);
        memcpy(buf1, buf, sizeof(buf));
        if ((int)n >= 0) {
            printf("Request received\n");
            comd = strtok_r(buf, " \t\r\n\v\f", &context);
            tgtpath = strtok_r(NULL, " \t\r\n\v\f", &context);
            httpver = strtok_r(NULL, " \t\r\n\v\f", &context);
            host = strtok_r(NULL, " \t\r\n\v\f", &context);
            host = strtok_r(NULL, " \t\r\n\v\f", &context);
            strcpy(url, host);
            strcat(url, tgtpath);
            // Based on the incoming header data, decide if the connection must be persistent or not.
            if (strcmp(httpver, "HTTP/1.1") == 0) {
                c = context[1];
                if (c != '\r') {
                    temp = strtok_r(NULL, " \t\r\n\v\f", &context);
                    temp = strtok_r(NULL, " \t\r\n\v\f", &context);
                    if (strcmp(temp, "Keep-alive") == 0 || strcmp(temp, "keep-alive") == 0) {
                        keepalive = 1;
                    } else {
                        keepalive = 0;
                    }
                } else {
                    keepalive = 1;
                }
            }
            printf("comd=%s tgtpath=%s httpver=%s host=%s keepalive=%d \n", comd, tgtpath, httpver, host, keepalive);
            // Choose what to perform based on comd
            if (strcmp(comd, "GET") == 0) {
                sendfd = open_sendfd(80, host);
                if (sendfd < 0) {
                    printf("sendfd < 0\n");
                    sprintf(msg, "<html><head><title>400 Bad Request</title></head><body><h2>400 Bad Request</h2></body></html>");
                    sprintf(resp, "%s 400 Bad Request\r\nContent-Type:text/html\r\nContent-Length:%d\r\n\r\n%s", httpver, (int)strlen(msg), msg);
                    write(connfd, resp, strlen(resp));
                } else {
                    fpath = checkCache(url);
                    if (strcmp(fpath, "") == 0) {
                        write(sendfd, buf1, sizeof(buf1));
                        bzero(resp, MAXBUF);
                        m = read(sendfd, resp, MAXBUF);
                        if (m < 0) {
                            printf("No response from server\n");
                            sprintf(msg, "<html><head><title>400 Bad Request</title></head><body><h2>400 Bad Request</h2></body></html>");
                            sprintf(resp, "%s 400 Bad Request\r\nContent-Type:text/html\r\nContent-Length:%d\r\n\r\n%s", httpver, (int)strlen(msg), msg);
                        } else {
                            printf("Store to cache\n");
                            found = 0;
                            for (i = 0; i < cacheLen; i++) {
                                if(strcmp(url, cacheList[i][0]) == 0) {
                                    //cacheList[i][2] = New time
                                    found = 1;
                                }
                            }
                            if (!found) {
                                strcpy(cacheList[cacheLen][0], url);
                            }
                        }
                    } else {
                        printf("Read from cached file\n");
                    }
                    write(connfd, resp, strlen(resp));
                }
            } else {
                // Process Internal Server errors like HTTP commands that are not supported
                sprintf(msg, "<html><head><title>400 Bad Request</title></head><body><h2>400 Bad Request</h2></body></html>");
                sprintf(resp, "%s 400 Bad Request\r\nContent-Type:text/html\r\nContent-Length:%d\r\n\r\n%s", httpver, (int)strlen(msg), msg);
                write(connfd, resp, strlen(resp));
            }
        } else {
            printf("No data received\n");
            keepalive = 0;
        }
    }
    printf("Closing thread\n");
    close(connfd);
    return NULL;
}

/* 
 * open_listenfd - open and return a listening socket on port
 * Returns -1 in case of failure 
 */
int open_listenfd(int port) {
    int listenfd, optval=1;
    struct sockaddr_in serveraddr;
  
    /* Create a socket descriptor */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    /* Eliminates "Address already in use" error from bind. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, 
                    (const void *)&optval , sizeof(int)) < 0)
        return -1;

    /* Sets a timeout of 10 secs. */
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    if (setsockopt(listenfd, SOL_SOCKET, SO_RCVTIMEO,
                    (struct timeval *)&tv,sizeof(struct timeval)) < 0)
        return -1;

    /* listenfd will be an endpoint for all requests to port
       on any IP address for this host */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET; 
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    serveraddr.sin_port = htons((unsigned short)port); 
    if (bind(listenfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;
    return listenfd;
} /* end open_listenfd */

/* 
 * open_sendfd - open and return a sending socket on port
 * Returns -1 in case of failure 
 */
int open_sendfd(int port, char *host) {
    int sendfd;
    struct sockaddr_in serveraddr;
    char *hostip;

    /* Create a socket descriptor */
    if ((sendfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    
    /* Sets a timeout of 10 secs. */
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    if (setsockopt(sendfd, SOL_SOCKET, SO_RCVTIMEO,
                    (struct timeval *)&tv,sizeof(struct timeval)) < 0)
        return -1;
    hostip = hostname_to_ip(host);
    if (strcmp(hostip, "error") == 0)
        return -1;
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET; 
    serveraddr.sin_addr.s_addr = inet_addr(hostip); 
    serveraddr.sin_port = htons((unsigned short)port);
    if (connect(sendfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;
    return sendfd;
} /* end open_sendfd */

char* hostname_to_ip(char *hostname) {
    struct hostent *he;
    struct in_addr **addr_list;
    char *ip = (char*) malloc (20 * sizeof(char));
    int i;

    if ((he = gethostbyname( hostname)) == NULL) {
        // get the host info
        herror("gethostbyname");
        return "error";
    }

    addr_list = (struct in_addr **) he->h_addr_list;

    for (i = 0; addr_list[i] != NULL; i++) {
        //Return the first one;
        strcpy(ip , inet_ntoa(*addr_list[i]) );
        return ip;
    }

    return ip;
}