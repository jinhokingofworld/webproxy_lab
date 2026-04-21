#include "csapp.h"

int main(int argc, char **argv) 
{ 
    int clientfd;
    char *host, *port, buf[MAXLINE];
    rio_t rio;

    //argv[0]은 프로그램 이름
    if (argc != 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        exit(0);
    }
    host = argv[1];
    port = argv[2];

    //open_clientfd의 래퍼함수
    clientfd = Open_clientfd(host, port);
    //rio 구조체 초기화
    Rio_readinitb(&rio, clientfd);

    //사용자의 입력이 없을 때까지 반복
    while (Fgets(buf, MAXLINE, stdin) != NULL) {
        Rio_writen(clientfd, buf, strlen(buf));
        Rio_readlineb(&rio, buf, MAXLINE);
        Fputs(buf, stdout);
    } 

    Close(clientfd);
    exit(0);
}
