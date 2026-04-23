/* $begin tinymain */
/*
 * tiny.c - 아주 작은 HTTP/1.0 웹 서버 예제입니다.
 *
 * 이 서버는 브라우저가 보낸 HTTP 요청을 읽고,
 *   1) HTML, 이미지 같은 "정적 콘텐츠(static content)"를 파일에서 읽어 보내거나
 *   2) CGI 프로그램을 실행해서 만든 "동적 콘텐츠(dynamic content)"를 보내는 일을 합니다.
 *
 * "iterative" 서버라는 말은 한 번에 클라이언트 하나씩만 처리한다는 뜻입니다.
 * 즉, 한 요청을 끝낸 뒤 다음 요청을 받습니다.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);

int main(int argc, char **argv)
{
  int listenfd, connfd;

  /* 접속한 클라이언트의 이름/IP와 포트 번호를 저장합니다.
   * MAXLINE은 csapp.h에 정의된 넉넉한 버퍼 크기입니다.*/
  char hostname[MAXLINE], port[MAXLINE];

  /*클라이언트 주소 구조체의 크기를 담습니다. 주소 타입 판별 용도*/
  socklen_t clientlen;

  /* sockaddr_storage는 IPv4와 IPv6 주소를 모두 담을 수 있는 큰 주소 구조체입니다.
   * accept 함수에서는 사용가능한 크기를 나타내는 역할을 한다.
   * 클라이언트가 어디서 접속했는지 저장합니다.*/
  struct sockaddr_storage clientaddr;

  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  // 사용자가 입력한 포트(argv[1])로 듣기 소켓을 엽니다.
  listenfd = Open_listenfd(argv[1]);

  while (1)
  {
    /* accept를 호출하기 전에 주소 구조체 크기를 알려줍니다. */
    clientlen = sizeof(clientaddr);

    connfd = Accept(listenfd, (SA *)&clientaddr,
                    &clientlen); // line:netp:tiny:accept

    /* 클라이언트 주소 정보를 사람이 읽기 좋은 문자열로 바꿉니다.
     * hostname에는 IP/호스트 이름, port에는 포트 번호가 들어갑니다.*/
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);

    /* 실제 HTTP 요청을 읽고 응답을 보내는 핵심 함수입니다. */
    doit(connfd);  // line:netp:tiny:doit

    /* 이 서버는 HTTP/1.0 방식처럼 응답 하나를 보낸 뒤 연결을 닫습니다. */
    Close(connfd); // line:netp:tiny:close
  }
}

void doit(int fd) {
  /* 1이면 정적 콘텐츠, 0이면 동적 콘텐츠를 뜻합니다. */
  int is_static;

  /* stat 함수가 파일의 크기, 권한, 종류 같은 정보를 여기에 채워줍니다. */
  struct stat sbuf;

   /* method는 GET 같은 HTTP 메서드,
   * uri는 /home.html 같은 요청 경로,
   * version은 HTTP/1.0 같은 프로토콜 버전을 저장합니다.*/
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  /*  (질문) 데이터들을 고정 배열로 정해서 넣는데, malloc으로 왜 안쓸까?
      1. 버퍼 크기 = 읽는 최대 크기 -> 버퍼 오버플로우 방지
      2. malloc보다 속도 빠름. 캐시 친화성 & 오버헤드 없음
        malloc을 써야 하는 경우
        1. 크기가 아주 클 때 (스택 오버플로우 위험)
        2. 크기를 모를 때 (동적 데이터) ex) 파일 업로드, 긴 HTTP body
        3. 오래 살아야 하는 데이터
      3. 현재는 HTTP 요청 자체가 짧음
      4. 무시 가능한 수준의 버퍼
  */

  /* filename은 실제 서버 파일 시스템에서 찾을 파일 이름입니다.
   * cgiargs는 CGI 프로그램에 넘길 쿼리 문자열입니다.*/
  char filename[MAXLINE], cgiargs[MAXLINE];

  /* rio_t는 Robust I/O 패키지가 사용하는 버퍼 구조체입니다.
   * 소켓에서 한 줄씩 안정적으로 읽기 위해 사용합니다.*/
  rio_t rio;

  /* fd와 rio 버퍼를 연결해서, 앞으로 rio를 통해 fd에서 읽을 수 있게 합니다. */
  rio_readinitb(&rio, fd);

  /* 클라이언트가 보낸 요청 첫 줄 ex) GET /index.html HTTP/1.0을 읽어서 buf에 저장합니다. */
  Rio_readlineb(&rio, buf, MAXLINE);

  /* 서버 터미널에 요청 첫 줄을 출력해 확인합니다. */
  printf("Request headers: ");
  printf("%s\n", buf);

  /* 요청 첫 줄을 공백 기준으로 나누어 method, uri, version에 저장합니다.
   * 예: "GET /home.html HTTP/1.1"이라면
   * method="GET", uri="/home.html", version="HTTP/1.1"이 됩니다.*/
  sscanf(buf, "%s %s %s", method, uri, version);

  /* Tiny 서버는 GET 요청만 처리합니다.
   * strcasecmp는 대소문자를 무시하고 문자열을 비교합니다.
   * 두 문자열이 같으면 0을 반환*/
  if (strcasecmp(method, "GET")) {
    /* GET이 아닌 POST, PUT 같은 요청은 아직 구현하지 않았다는 에러를 보냅니다. */
    clienterror(fd, method, "501", "Not implemented",
                "Tiny does not implement this method");
    return; 
  }

  /* 나머지 HTTP 헤더들을 읽습니다.
   * 이 예제 서버는 헤더 내용을 실제로 사용하지 않고 읽어서 버립니다.*/
  read_requesthdrs(&rio);

  /* Parse URI from GET request
   * uri를 분석해서 실제 파일 이름(filename)과 CGI 인자(cgiargs)를 만듭니다.*/
  is_static = parse_uri(uri, filename, cgiargs);

  /* stat은 filename 파일이 실제로 있는지 확인하고, 있으면 파일 정보를 sbuf에 넣습니다.
   * 0보다 작으면 파일을 찾지 못했거나 접근할 수 없다는 뜻입니다.*/
  if (stat(filename, &sbuf) < 0) {
    /* 요청한 파일이 없으면 404 Not found 응답을 보냅니다. */
    clienterror(fd, filename, "404", "Not found",
                "Tiny couldn't find this file");
    return;
  }

  if (is_static) { /* Serve static content */
    /* 정적 콘텐츠는 일반 파일이어야 하고, 서버가 읽을 권한이 있어야 합니다.
     * S_ISREG는 "일반 파일인가?"를 검사합니다.
     * S_IRUSR는 "파일 소유자가 읽을 수 있는가?"를 나타내는 권한 비트입니다.
     */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
      /* 파일이 일반 파일이 아니거나 읽을 권한이 없으면 403 Forbidden을 보냅니다. */
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny couldn't read the file");
      return;
    }

    /* 파일을 읽어 클라이언트에게 보냅니다.
     * sbuf.st_size는 파일 크기입니다.
     */
    serve_static (fd, filename, sbuf.st_size);
  }
  else { /* Serve dynamic content */
    /* 동적 콘텐츠는 실행 가능한 CGI 프로그램이어야 합니다.
     * S_IXUSR는 "파일 소유자가 실행할 수 있는가?"를 나타내는 권한 비트입니다.
     */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
      /* 실행 파일이 아니거나 실행 권한이 없으면 403 Forbidden을 보냅니다. */
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny couldn't run the CGI program");
      return;
    }

    /* CGI 프로그램을 실행해서 그 결과를 클라이언트에게 보냅니다. */
    serve_dynamic (fd, filename, cgiargs);
  }
}

/* HTTP 요청 헤더를 끝까지 읽는 함수입니다.
 * 이 Tiny 서버는 헤더 정보를 사용하지 않으므로, 읽고 출력한 뒤 버립니다.
 * HTTP 헤더는 빈 줄 "\r\n"이 나오면 끝납니다.
 */
void read_requesthdrs(rio_t *rp)
{
  /* 헤더 한 줄을 담을 임시 버퍼입니다. */
  char buf[MAXLINE];

  /* 첫 번째 헤더 줄을 읽습니다. */
  Rio_readlineb(rp, buf, MAXLINE);

  /* HTTP 헤더는 빈 줄("\r\n")이 나올 때까지 이어집니다.
   * strcmp가 0이면 두 문자열이 같다는 뜻이므로,
   * while(strcmp(buf, "\r\n"))는 "빈 줄이 아닐 동안 반복"입니다.
   */
  while(strcmp(buf, "\r\n")) {
    /* 다음 헤더 줄을 계속 읽습니다. */
    rio_readlineb(rp, buf, MAXLINE);

    /* 읽은 헤더를 서버 터미널에 출력합니다. */
    printf("%s", buf);
  }

  /* void 함수라서 꼭 return이 필요하지는 않지만,
   * 여기서는 함수가 끝났음을 명확히 보여줍니다.
   */
  return;
}

/* URI를 분석해서 정적 콘텐츠인지 동적 콘텐츠인지 판단합니다.
 *
 * uri: 브라우저가 요청한 경로입니다. 예: /home.html, /cgi-bin/adder?1&2
 * filename: 서버 디스크에서 실제로 열 파일 경로를 저장할 곳입니다.
 * cgiargs: CGI 프로그램에 넘길 인자 문자열을 저장할 곳입니다.
 *
 * 반환값:
 *   1이면 정적 콘텐츠
 *   0이면 동적 콘텐츠
 */
int parse_uri(char *uri, char *filename, char *cgiargs)
{
  /* ptr은 URI 안에서 '?' 위치를 가리킬 포인터입니다.
   * '?' 뒤쪽은 CGI 프로그램에 넘길 쿼리 문자열입니다.
   */
  char *ptr;

  /* 이 코드에서는 URI에 "cgi-btn"이라는 문자열이 없으면 정적 콘텐츠로 봅니다.
   * 일반 CS:APP 예제는 "cgi-bin"을 기준으로 나누는 경우가 많습니다.
   */
  if (!strstr(uri, "cgi-btn")) { /* Static content */
    /* 정적 파일에는 CGI 인자가 필요 없으므로 빈 문자열로 둡니다. */
    strcpy(cgiargs, "");

    /* filename을 현재 디렉터리 "."에서 시작하게 만듭니다.
     * 예를 들어 uri가 /home.html이면 filename은 ./home.html이 됩니다.
     */
    strcpy(filename, ".");

    /* 요청 URI를 filename 뒤에 붙입니다. */
    strcat(filename, uri);

    /* URI가 /로 끝나면 디렉터리를 요청한 것입니다.
     * 이 경우 기본 페이지 home.html을 붙여서 ./home.html 같은 파일을 찾습니다.
     */
    if (uri[strlen(uri)-1] == '/')
      strcat(filename, "home.html");

    /* 정적 콘텐츠라는 뜻으로 1을 반환합니다. */
    return 1;
  }
  else { /* Dynamic content */
    /* 동적 콘텐츠 URI에서 '?'를 찾습니다.
     * 예: /cgi-bin/adder?1&2 에서 ptr은 '?' 위치를 가리킵니다.
     */

    //index는 BSD계열 함수라 Unix기반 운영체제에서만 작동한다.
    //char *strchr(const char *s, int c); c 기본 함수를 사용하는 것이 이식성이 좋다. 
    ptr = index(uri, '?');

    /* '?'가 있다면 그 뒤쪽 문자열을 CGI 인자로 분리합니다. */
    if (ptr) {
      /* ptr+1은 '?' 바로 다음 글자입니다.
       * 예: ?1&2 라면 cgiargs에는 "1&2"가 들어갑니다.
       */
      strcpy(cgiargs, ptr+1);

      /* '?' 자리를 문자열 끝('\0')으로 바꿔서,
       * uri가 프로그램 경로까지만 남도록 자릅니다.
       */
      *ptr = '\0';
    }
    else
      /* '?'가 없으면 CGI 인자는 빈 문자열입니다. */
      strcpy(cgiargs, "");

    /* 동적 콘텐츠도 현재 디렉터리 기준 경로로 만들기 위해 "."을 넣습니다. */
    strcpy(filename, ".");

    /* 현재 코드에서는 바로 다음 줄에서 filename을 uri로 덮어씁니다.
     * 원래 의도가 현재 디렉터리 기준 경로라면 strcat(filename, uri)가 되어야 합니다.
     * 여기서는 코드 동작을 바꾸지 않고, 현재 작성된 흐름을 설명만 남깁니다.
     */
    strcpy(filename, uri);

    /* 동적 콘텐츠라는 뜻으로 0을 반환합니다. */
    return 0;
  }
}

/* 정적 파일을 클라이언트에게 보내는 함수입니다.
 *
 * fd: 클라이언트와 연결된 소켓
 * filename: 보낼 파일 이름
 * filesize: 보낼 파일의 크기
 */
void serve_static(int fd, char *filename, int filesize)
{
  /* srcfd는 디스크 파일을 열었을 때 얻는 파일 디스크립터입니다. */
  int srcfd;

  /* srcp는 mmap으로 메모리에 연결한 파일 내용을 가리킵니다.
   * filetype은 MIME 타입을 저장합니다. 예: text/html, image/png
   * buf는 HTTP 응답 헤더를 만들 때 쓰는 버퍼입니다.*/
  char *srcp, filetype[MAXLINE], buf[MAXBUF];

  /* Send response headers to client
   * 먼저 클라이언트에게 "응답이 성공했고, 어떤 종류의 데이터가 갈지" 알려줍니다.
   * 파일 확장자를 보고 Content-type에 넣을 MIME 타입을 정합니다. */
  get_filetype(filename, filetype);

  /*sprintf는 char배열에 넣겠다는 뜻
    교재의 코드는 sprintf를 사용했지만, 버퍼 크기를 생각하지 않고, 정의되지 않은 동작(UB)을 할 수 있는 코드다.
    strcat(이것도 버퍼 크기 체크 안함)이나, sprintf 위치 이동 하는 것이 좋다.
    현대에서는 snprintf를 사용하는 것이 최고.
  */

  /* HTTP 응답의 상태 줄입니다. 200 OK는 요청이 성공했다는 뜻입니다. */
  sprintf(buf, "HTTP/1.0 200 OK\r\n");

  /* Server 헤더는 응답을 보내는 서버 이름을 알려줍니다. */
  sprintf(buf + strlen(buf), "Server: Tiny Web Server\r\n");

  /* Connection: close는 응답 후 연결을 닫겠다는 뜻입니다. */
  sprintf(buf + strlen(buf), "Connection: close\r\n");

  /* Content-length는 응답 본문(body)의 바이트 수입니다.
   * 브라우저는 이 값을 보고 데이터를 얼마나 읽어야 할지 알 수 있습니다.*/
  sprintf(buf + strlen(buf), "Content-length: %d\r\n", filesize);

  /* Content-type은 본문 데이터가 HTML인지, 이미지인지 등을 알려줍니다. */
  sprintf(buf + strlen(buf), "Content-type: %s\r\n\r\n", filetype);

  /* 완성한 HTTP 응답 헤더를 클라이언트에게 보냅니다. */
  Rio_writen(fd, buf, strlen(buf));

  /* 서버 터미널에도 응답 헤더를 출력해 디버깅할 수 있게 합니다. */
  printf("Response headers:\n");
  printf("%s", buf);

  /* Send response body to client
   * 이제 실제 파일 내용을 응답 본문으로 보냅니다.*/

  /* filename 파일을 읽기 전용으로 엽니다. */
  srcfd = Open(filename, O_RDONLY, 0);

  /* mmap은 파일 내용을 메모리에 매핑합니다.
   * 쉽게 말해, 파일 전체를 배열처럼 읽을 수 있게 해줍니다.
   * PROT_READ는 읽기만 하겠다는 뜻이고,
   * MAP_PRIVATE는 이 메모리를 수정해도 원본 파일에는 반영하지 않겠다는 뜻입니다.
   */
  srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);

  /* mmap을 한 뒤에는 파일 디스크립터를 닫아도 매핑된 메모리는 사용할 수 있습니다. */
  Close(srcfd);

  /* 메모리에 올라온 파일 내용을 클라이언트에게 그대로 보냅니다. */
  Rio_writen(fd, srcp, filesize);

  /* 사용이 끝난 메모리 매핑을 해제합니다. */
  Munmap(srcp, filesize);
}

/* 파일 이름의 확장자를 보고 HTTP Content-Type 값을 정하는 함수입니다.
 * 브라우저는 이 타입을 보고 받은 데이터를 어떻게 해석할지 결정합니다.
 */
void get_filetype(char *filename, char *filetype)
{
  /* 파일 이름에 .html이 들어 있으면 HTML 문서로 판단합니다. */
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  /* GIF 이미지입니다. */
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  /* PNG 이미지입니다. */
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  /* JPEG 이미지입니다. */
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  /* 위 확장자에 해당하지 않으면 일반 텍스트로 보냅니다. */
  else
    strcpy(filetype, "text/plain");
}

/* 동적 콘텐츠를 처리하는 함수입니다.
 * CGI 프로그램을 실행하고, 그 프로그램의 출력이 클라이언트에게 가도록 만드는 역할입니다.
 */
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
  /* buf는 응답 헤더를 만들 때 쓰는 버퍼입니다.
   * emptylist는 execve에 넘길 인자 배열입니다.
   * 여기서는 별도 명령줄 인자를 넘기지 않기 때문에 NULL 하나만 둡니다.
   */
  char buf[MAXLINE], *emptylist[] = { NULL };

  /* Return first aprt of HTTP response
   * 동적 콘텐츠의 HTTP 응답 헤더 앞부분을 먼저 보냅니다.
   * 오타가 있지만 원래 주석을 유지하면 "first part"라는 뜻입니다.
   */

  /* 성공 상태 줄을 클라이언트에게 보냅니다. */
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  rio_writen(fd, buf, strlen(buf));

  /* 서버 이름 헤더를 보냅니다. */
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  /* Fork는 현재 프로세스를 복사해서 자식 프로세스를 만듭니다.
   * 부모 프로세스에는 자식 PID가 반환되고,
   * 자식 프로세스에는 0이 반환됩니다.
   * 그래서 if (Fork() == 0)은 "자식 프로세스라면"이라는 뜻입니다.
   */
  if (Fork() == 0) { /* Child */
    /* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1);
    Dup2(fd, STDOUT_FILENO);
    Execve(filename, emptylist, environ);
  }
  Wait(NULL);
}

/* 클라이언트에게 HTTP 에러 응답을 보내는 함수입니다.
 *
 * cause: 에러를 일으킨 대상입니다. 예: 파일 이름이나 HTTP 메서드
 * errnum: HTTP 상태 코드입니다. 예: 404, 403, 501
 * shortmsg: 짧은 에러 메시지입니다. 예: Not found
 * longmsg: 조금 더 자세한 설명입니다.
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg) 
{
  /* buf는 HTTP 응답 헤더를 만들 때 사용합니다.
   * body는 브라우저 화면에 표시할 HTML 본문을 만들 때 사용합니다.
   */
  char buf[MAXLINE], body[MAXBUF];

  /* Build the HTTP response body
   * 브라우저가 보여줄 간단한 HTML 에러 페이지를 문자열로 만듭니다.
   */

  /* HTML 문서의 시작과 제목을 만듭니다. */
  sprintf(body, "<html><title>Tiny Error</title>");

  /* body 태그를 추가합니다.
   * 여기서 ""ffffff""는 C 문자열 안에서 따옴표를 표현하려고 쓴 형태입니다.
   */
  sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);

  /* 예: "404: Not found" 같은 한 줄을 본문에 추가합니다. */
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);

  /* 더 자세한 에러 설명과 원인을 본문에 추가합니다. */
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);

  /* 페이지 아래쪽에 서버 이름을 표시합니다. */
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* Print the HTTP response
   * 이제 HTTP 응답 헤더를 먼저 보내고, 이어서 HTML 본문을 보냅니다.
   */

  /* 상태 줄입니다. 예: HTTP/1.0 404 Not found */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf));

  /* 에러 본문은 HTML이므로 Content-type을 text/html로 보냅니다. */
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));

  /* 본문 길이를 알려주고, 빈 줄 \r\n\r\n으로 헤더의 끝을 표시합니다. */
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));

  /* 마지막으로 실제 HTML 에러 페이지 본문을 보냅니다. */
  Rio_writen(fd, body, strlen(body));
}
