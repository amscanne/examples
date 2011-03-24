/*
 * httpd.c
 *
 * Copyright Adin Scannell 2011
 *
 * This code is intended for demo purposes only, 
 * please don't actually use it to do anything.
 *
 * To compile, use:
 *   gcc -o httpd httpd.c

 * Then, create a file 'index.html' in the directory
 * that you are running the server from. Put stuff in
 * it.

 * Then go to http://localhost:8888 and see the file.
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define HTTP_BAD_REQUEST "HTTP/1.0 404 Not Found\r\n"
#define HTTP_GOOD_REQUEST "HTTP/1.0 200\r\n"
#define HTTP_CONTENT_TYPE "Content-Type: text/html\r\n"
#define HTTP_CONTENT_LENGTH "Content-Length: %ld\r\n\r\n"
#define MAX_REQUEST_SIZE 4096
#define DEFAULT_PATH "index.html"

#ifdef DEBUG
#define log(fmt, args...) fprintf(stderr, fmt "\n", ## args)
#else
#define log(fmt, args...) 
#endif

int send_error(int fd) {
    write(fd, HTTP_BAD_REQUEST, strlen(HTTP_BAD_REQUEST));
    write(fd, HTTP_CONTENT_TYPE, strlen(HTTP_CONTENT_TYPE));
    return 0;
}

int send_file(int fd, char* path) {
    int iofd = open(path, O_RDONLY);
    if( iofd < 0 ) {
        write(fd, HTTP_BAD_REQUEST, strlen(HTTP_BAD_REQUEST));
        write(fd, HTTP_CONTENT_TYPE, strlen(HTTP_CONTENT_TYPE));
        log("error opening file");
    } else {
        char length[MAX_REQUEST_SIZE];
        struct stat statinfo;
        if( fstat(iofd, &statinfo) < 0 ) {
            /* Can't get the length. */
            write(fd, HTTP_BAD_REQUEST, strlen(HTTP_BAD_REQUEST));
            write(fd, HTTP_CONTENT_TYPE, strlen(HTTP_CONTENT_TYPE));
            log("error fetching size");
        } else {
            write(fd, HTTP_GOOD_REQUEST, strlen(HTTP_BAD_REQUEST));
            write(fd, HTTP_CONTENT_TYPE, strlen(HTTP_CONTENT_TYPE));
            snprintf(length, MAX_REQUEST_SIZE,
                     HTTP_CONTENT_LENGTH, statinfo.st_size);
            if( write(fd, length, strlen(length)) < 0 ||
                sendfile(fd, iofd, NULL, statinfo.st_size) < 0 ) {
                /* Error sending the file. Nothing can be done. */
                perror("error writing file");
            }
        }

        close(iofd);
    }

    return 0;
}

int process(int fd, char* header) {
    char npath[MAX_REQUEST_SIZE];

    char *eol = strchr(header, '\r');

    /* Nuts to thread-safe, this demo uses it's own process :P */
    char *method = strtok(header, " ");
    char *path = strtok(NULL, " ");
    char *http = strtok(NULL, " ");

    if( eol != NULL ) {
        *eol = '\0';
    }

    /* Debug output here, just in case anyone is watching. */
    log(" * method = %s", method);
    log(" * path = %s", path);
    log(" * http = %s", http);

    /* Ensure that we can process it. */
    if( strcmp(method, "GET") ||
       (strcmp(http, "HTTP/1.0") && strcmp(http, "HTTP/1.1")) ) {
        log("bad request");
        return send_error(fd);
    } else {
        if( path[0] == '/' && path[1] == '\0' ) {
            path = DEFAULT_PATH;
        } else if( path[0] == '/' ) {
            snprintf(npath, MAX_REQUEST_SIZE, ".%s", path);
            path = npath;
        }
        log("sending %s", path);
        return send_file(fd, path);
    }
}

int service(int fd) {
    char buffer[MAX_REQUEST_SIZE];
    int readbytes = 0, scanned = 0;

    /* Attempt to read a chunk of bytes, but not process forever. */
    while( readbytes < (MAX_REQUEST_SIZE-1) ) {
        int cur = read(fd, &(buffer[readbytes]), (MAX_REQUEST_SIZE-1) - readbytes);
        if( cur < 0 ) {
            perror("read failed");
            return -1;
        } else {
            buffer[readbytes + cur + 1] = '\0';
            readbytes += cur;
        }

        /* Check for the \n\n found at the end of the header. */
        for( ; scanned < readbytes-3; scanned++ ){
            if( buffer[scanned] == '\r' && buffer[scanned+1] == '\n' &&
                buffer[scanned+2] == '\r' && buffer[scanned+3] == '\n' ) {
                buffer[scanned] = '\0';
                return process(fd, buffer);
            }
        }
    }
   
    /* Failed to find the end of the header. */
    log("header too long");
    return -1;
}

int main(int argc, char *argv[]) {

    struct sockaddr_in servaddr;
    int serversock = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;

    /*  Create socket. */
    if( serversock < 0 ) {
	    perror("error creating socket");
        return 1;
    }

    /* Tweak it for reuse. Not necessary for fork, just useful  *
     * when one is killing and running this server a few times. */
    if( setsockopt(serversock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0 ) {
        perror("unable to tweak socket options");
    }
    
    /* Bind to port 8888. */
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(8888);
    if( bind(serversock, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0 ) {
        perror("couldn't bind to given address");
        return 1;
    }
    log("bound to 0.0.0.0:8888");

    /* Start listening, queue size 10. */
    if( listen(serversock, 10) < 0 ) {
        perror("listen failed");
        return 1;
    }
    log("listening");

    /* Enter the service loop. */
    while(1) {
        int clientsock, pid, childstatus;
        log("waiting for next request");

        /* Grab the next request. */
        clientsock = accept(serversock, NULL, NULL);
        if( clientsock < 0 ) {
            perror("accept failed");
            break;
        }
        log("accepted connection, forking");

        /* Fork off a handler. */
        pid = fork();
        if( pid < 0 ) {
            perror("fork failed");
            close(clientsock);
            continue;
        } else if( pid > 0 ) {
            close(clientsock);
        } else {
            close(serversock);
            log("servicing connection (as child)");
            service(clientsock);
            return 0;
        }

        /* Collect any exit statuses (lazy). */
        while(1) {
            pid = waitpid(-1, &childstatus, WNOHANG);
            if( pid < 0 ) {
                perror("waitpid error?");
                break;
            } else if( pid == 0 ) {
                break;
            } else if( WIFEXITED(childstatus) ) {
                log("child %d exited with status %d", pid, WEXITSTATUS(childstatus));
            }
        }
	}

    /* Should never arrive here. */
    return 254;
}
