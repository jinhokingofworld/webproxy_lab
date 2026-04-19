#include "csapp.h"

void echo(int connfd);

int main(int argc, char **argv) {

    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr; /* Enough space for any address */
    char client_hostname[MAXLINE], client_port[MAXLINE];

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }

    listenfd = Open_listenfd(argv[1]);
    //서버는 일을 쉬지 않는다.
    while (1) {
        //int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
        //accept(listenfd, (SA *)&clientaddr, &clientlen);
        //clientlen은 커널이 클라이언트의 주소를 써줄 공간이다.
        clientlen = sizeof(struct sockaddr_storage);

        // typedef struct sockaddr SA;
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

        //SA 구조체는 사람이 보기 힘들어서 변환이 필요함
        //마지막 인자는 flag. 0은 기본동작
        Getnameinfo((SA *) &clientaddr, clientlen, client_hostname, MAXLINE,
            client_port, MAXLINE, 0);
        printf("Connected to (%s %s)\n", client_hostname, client_port);

        echo(connfd);
        Close(connfd);
    }
    exit (0);
}

void echo (int connfd) {
    
    size_t n;
    char buf[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);

    while((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
        printf("server received %d bytes\n", (int) n);
        Rio_writen(connfd, buf, n);
    }
}
