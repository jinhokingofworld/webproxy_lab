/* 
 * csapp.c - Functions for the CS:APP3e book
 *
 * Updated 10/2016 reb:
 *   - Fixed bug in sio_ltoa that didn't cover negative numbers
 *
 * Updated 2/2016 droh:
 *   - Updated open_clientfd and open_listenfd to fail more gracefully
 *
 * Updated 8/2014 droh: 
 *   - New versions of open_clientfd and open_listenfd are reentrant and
 *     protocol independent.
 *
 *   - Added protocol-independent inet_ntop and inet_pton functions. The
 *     inet_ntoa and inet_aton functions are obsolete.
 *
 * Updated 7/2014 droh:
 *   - Aded reentrant sio (signal-safe I/O) routines
 * 
 * Updated 4/2013 droh: 
 *   - rio_readlineb: fixed edge case bug
 *   - rio_readnb: removed redundant EINTR check
 */
/* $begin csapp.c */
#include "csapp.h"

/*
 * 학습용 안내: csapp.c는 CS:APP 책에서 제공하는 "도우미 함수 모음"입니다.
 *
 * 이 파일의 가장 큰 역할은 운영체제 함수들을 조금 더 쓰기 쉽게 감싸는 것입니다.
 * 예를 들어 read(), write(), socket(), bind(), accept() 같은 함수는 실패하면
 * 보통 -1을 반환하고, 실패 이유는 errno라는 전역 변수에 남깁니다. 매번
 * 에러 검사를 직접 쓰면 실습 코드가 지저분해지므로, 이 파일은 Read(),
 * Write(), Socket(), Bind(), Accept()처럼 첫 글자가 대문자인 래퍼 함수를
 * 제공합니다. 대문자 래퍼는 실패하면 에러 메시지를 출력하고 종료합니다.
 *
 * 이름 읽는 법:
 *   - open, read, socket: 운영체제가 제공하는 원래 함수
 *   - Open, Read, Socket: 이 파일이 제공하는 에러 처리 포함 래퍼
 *   - rio_...: robust I/O 원본 함수. 실패를 반환값으로 알려줍니다.
 *   - Rio_...: rio_...에 에러 처리를 붙인 래퍼
 *
 * rc라는 변수 이름:
 *   이 파일에는 int rc;가 많이 나옵니다. rc는 보통 "return code"의 줄임말입니다.
 *   함수 호출 결과를 잠깐 담아두고, 성공/실패를 검사하기 위한 임시 변수입니다.
 *
 *     if ((rc = close(fd)) < 0)
 *         unix_error("Close error");
 *
 *   위 코드는 close(fd)의 결과를 rc에 저장한 다음, 그 값이 0보다 작으면
 *   실패로 보고 에러 처리한다는 뜻입니다. rc 자체가 특별한 문법은 아니고,
 *   result나 ret처럼 개발자들이 자주 쓰는 변수 이름일 뿐입니다.
 *
 * 소켓을 처음 볼 때 붙잡을 핵심:
 *   Unix/Linux에서는 파일, 터미널, 파이프, 네트워크 연결을 모두
 *   "파일 디스크립터(fd)"라는 정수 번호로 다룹니다. 소켓도 fd입니다.
 *   그래서 TCP 연결이 만들어진 뒤에는 파일처럼 read/write 할 수 있습니다.
 *
 * 웹 프록시 실습에서 자주 만나는 흐름:
 *   브라우저가 프록시에 접속할 수 있게 포트를 엽니다.
 *     listenfd = Open_listenfd(port)
 *     connfd = Accept(listenfd, ...)
 *
 *   프록시가 실제 웹 서버에 접속합니다.
 *     serverfd = Open_clientfd(hostname, port)
 *
 *   연결된 소켓 fd에서 HTTP 메시지를 읽고 씁니다.
 *     Rio_readinitb(&rio, fd)
 *     Rio_readlineb(&rio, buf, MAXLINE)
 *     Rio_writen(fd, buf, strlen(buf))
 */

/************************** 
 * Error-handling functions
 **************************/
/*
 * 에러 처리 함수들
 *
 * 시스템 프로그래밍에서는 함수 계열마다 에러를 알려주는 방식이 조금 다릅니다.
 *
 *   Unix 시스템 콜:
 *     실패하면 보통 -1을 반환하고 errno에 이유를 저장합니다.
 *     예: open, read, write, socket, bind, accept
 *
 *   POSIX pthread 함수:
 *     성공하면 0, 실패하면 에러 번호 자체를 반환합니다.
 *
 *   getaddrinfo 함수:
 *     성공하면 0, 실패하면 gai_strerror로 해석해야 하는 에러 코드를 반환합니다.
 *
 * 아래 함수들은 각 에러 코드를 사람이 읽을 수 있는 메시지로 바꿔 출력한 뒤
 * 프로그램을 종료합니다.
 */
/* $begin errorfuns */
/* $begin unixerror */
void unix_error(char *msg) /* Unix-style error */
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(0);
}
/* $end unixerror */

void posix_error(int code, char *msg) /* Posix-style error */
{
    fprintf(stderr, "%s: %s\n", msg, strerror(code));
    exit(0);
}

void gai_error(int code, char *msg) /* Getaddrinfo-style error */
{
    fprintf(stderr, "%s: %s\n", msg, gai_strerror(code));
    exit(0);
}

void app_error(char *msg) /* Application error */
{
    fprintf(stderr, "%s\n", msg);
    exit(0);
}
/* $end errorfuns */

void dns_error(char *msg) /* Obsolete gethostbyname error */
{
    fprintf(stderr, "%s\n", msg);
    exit(0);
}


/*********************************************
 * Wrappers for Unix process control functions
 ********************************************/
/*
 * 프로세스 제어 래퍼들
 *
 * fork/exec/wait는 프로세스를 만들고, 다른 프로그램을 실행하고, 자식 프로세스가
 * 끝나기를 기다리는 함수들입니다. webproxy lab의 중심은 소켓이지만, CS:APP
 * 전체 책에서 쓰는 공통 도우미라서 이 파일에 함께 들어 있습니다.
 */

/* $begin forkwrapper */
pid_t Fork(void) 
{
    pid_t pid;

    if ((pid = fork()) < 0)
	unix_error("Fork error");
    return pid;
}
/* $end forkwrapper */

void Execve(const char *filename, char *const argv[], char *const envp[]) 
{
    if (execve(filename, argv, envp) < 0)
	unix_error("Execve error");
}

/* $begin wait */
pid_t Wait(int *status) 
{
    pid_t pid;

    if ((pid  = wait(status)) < 0)
	unix_error("Wait error");
    return pid;
}
/* $end wait */

pid_t Waitpid(pid_t pid, int *iptr, int options) 
{
    pid_t retpid;

    if ((retpid  = waitpid(pid, iptr, options)) < 0) 
	unix_error("Waitpid error");
    return(retpid);
}

/* $begin kill */
void Kill(pid_t pid, int signum) 
{
    int rc;

    if ((rc = kill(pid, signum)) < 0)
	unix_error("Kill error");
}
/* $end kill */

void Pause() 
{
    (void)pause();
    return;
}

unsigned int Sleep(unsigned int secs) 
{
    unsigned int rc;

    if ((rc = sleep(secs)) < 0)
	unix_error("Sleep error");
    return rc;
}

unsigned int Alarm(unsigned int seconds) {
    return alarm(seconds);
}
 
void Setpgid(pid_t pid, pid_t pgid) {
    int rc;

    if ((rc = setpgid(pid, pgid)) < 0)
	unix_error("Setpgid error");
    return;
}

pid_t Getpgrp(void) {
    return getpgrp();
}

/************************************
 * Wrappers for Unix signal functions 
 ***********************************/
/*
 * 시그널 래퍼들
 *
 * 시그널은 운영체제가 프로세스에게 보내는 비동기 알림입니다.
 * 예: Ctrl-C는 SIGINT, 알람은 SIGALRM, 자식 종료 알림은 SIGCHLD입니다.
 *
 * 네트워크 서버에서는 accept/read/write 같은 시스템 콜이 시그널 때문에 중간에
 * 끊길 수 있습니다. 이때 errno가 EINTR이 됩니다. 아래 Signal 래퍼는
 * SA_RESTART 옵션을 켜서 가능한 경우 끊긴 시스템 콜이 자동으로 재시작되게
 * 설정합니다.
 */

/* $begin sigaction */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    /*
     * struct sigaction은 csapp.c가 직접 정의한 구조체가 아닙니다.
     * csapp.h가 include하는 <signal.h>에 들어 있는 POSIX 구조체입니다.
     *
     * action: 새로 등록할 시그널 처리 규칙
     * old_action: 이전에 등록되어 있던 처리 규칙을 돌려받는 공간
     */
    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* Block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* Restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}
/* $end sigaction */

void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    if (sigprocmask(how, set, oldset) < 0)
	unix_error("Sigprocmask error");
    return;
}

void Sigemptyset(sigset_t *set)
{
    if (sigemptyset(set) < 0)
	unix_error("Sigemptyset error");
    return;
}

void Sigfillset(sigset_t *set)
{ 
    if (sigfillset(set) < 0)
	unix_error("Sigfillset error");
    return;
}

void Sigaddset(sigset_t *set, int signum)
{
    if (sigaddset(set, signum) < 0)
	unix_error("Sigaddset error");
    return;
}

void Sigdelset(sigset_t *set, int signum)
{
    if (sigdelset(set, signum) < 0)
	unix_error("Sigdelset error");
    return;
}

int Sigismember(const sigset_t *set, int signum)
{
    int rc;
    if ((rc = sigismember(set, signum)) < 0)
	unix_error("Sigismember error");
    return rc;
}

int Sigsuspend(const sigset_t *set)
{
    int rc = sigsuspend(set); /* always returns -1 */
    if (errno != EINTR)
        unix_error("Sigsuspend error");
    return rc;
}

/*************************************************************
 * The Sio (Signal-safe I/O) package - simple reentrant output
 * functions that are safe for signal handlers.
 *************************************************************/
/*
 * Sio(signal-safe I/O)
 *
 * 시그널 핸들러 안에서는 printf 같은 함수를 마음대로 호출하면 위험할 수 있습니다.
 * 내부에서 전역 버퍼나 락을 건드릴 수 있기 때문입니다. 그래서 이 패키지는
 * write처럼 시그널 핸들러에서 비교적 안전한 함수만 사용해서 문자열과 숫자를
 * 출력합니다.
 */

/* Private sio functions */

/* $begin sioprivate */
/* sio_reverse - Reverse a string (from K&R) */
static void sio_reverse(char s[])
{
    int c, i, j;

    for (i = 0, j = strlen(s)-1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}

/* sio_ltoa - Convert long to base b string (from K&R) */
static void sio_ltoa(long v, char s[], int b) 
{
    int c, i = 0;
    int neg = v < 0;

    if (neg)
	v = -v;

    do {  
        s[i++] = ((c = (v % b)) < 10)  ?  c + '0' : c - 10 + 'a';
    } while ((v /= b) > 0);

    if (neg)
	s[i++] = '-';

    s[i] = '\0';
    sio_reverse(s);
}

/* sio_strlen - Return length of string (from K&R) */
static size_t sio_strlen(char s[])
{
    int i = 0;

    while (s[i] != '\0')
        ++i;
    return i;
}
/* $end sioprivate */

/* Public Sio functions */
/* $begin siopublic */

ssize_t sio_puts(char s[]) /* Put string */
{
    return write(STDOUT_FILENO, s, sio_strlen(s)); //line:csapp:siostrlen
}

ssize_t sio_putl(long v) /* Put long */
{
    char s[128];
    
    sio_ltoa(v, s, 10); /* Based on K&R itoa() */  //line:csapp:sioltoa
    return sio_puts(s);
}

void sio_error(char s[]) /* Put error message and exit */
{
    sio_puts(s);
    _exit(1);                                      //line:csapp:sioexit
}
/* $end siopublic */

/*******************************
 * Wrappers for the SIO routines
 ******************************/
ssize_t Sio_putl(long v)
{
    ssize_t n;
  
    if ((n = sio_putl(v)) < 0)
	sio_error("Sio_putl error");
    return n;
}

ssize_t Sio_puts(char s[])
{
    ssize_t n;
  
    if ((n = sio_puts(s)) < 0)
	sio_error("Sio_puts error");
    return n;
}

void Sio_error(char s[])
{
    sio_error(s);
}

/********************************
 * Wrappers for Unix I/O routines
 ********************************/
/*
 * Unix I/O 래퍼들
 *
 * Unix/Linux는 많은 입출력 대상을 fd(file descriptor)라는 정수로 표현합니다.
 * 파일 fd, 터미널 fd, 파이프 fd, 소켓 fd가 모두 같은 read/write/close 함수로
 * 다뤄질 수 있습니다.
 *
 *   read(fd, buf, count):
 *     fd에서 최대 count바이트를 읽어 buf에 넣습니다.
 *
 *   write(fd, buf, count):
 *     buf에 있는 count바이트를 fd로 보냅니다.
 *
 *   close(fd):
 *     fd를 닫고 운영체제 자원을 반납합니다.
 *
 * 소켓을 처음 배울 때 중요한 점은 "연결된 TCP 소켓도 fd라서 read/write가 된다"는
 * 것입니다. 프록시는 브라우저와 연결된 connfd에서 읽고, 서버와 연결된 serverfd에
 * 쓰는 식으로 동작합니다.
 */

int Open(const char *pathname, int flags, mode_t mode) 
{
    int rc;

    if ((rc = open(pathname, flags, mode))  < 0)
	unix_error("Open error");
    return rc;
}

ssize_t Read(int fd, void *buf, size_t count) 
{
    ssize_t rc;

    if ((rc = read(fd, buf, count)) < 0) 
	unix_error("Read error");
    return rc;
}

ssize_t Write(int fd, const void *buf, size_t count) 
{
    ssize_t rc;

    if ((rc = write(fd, buf, count)) < 0)
	unix_error("Write error");
    return rc;
}

off_t Lseek(int fildes, off_t offset, int whence) 
{
    off_t rc;

    if ((rc = lseek(fildes, offset, whence)) < 0)
	unix_error("Lseek error");
    return rc;
}

void Close(int fd) 
{
    int rc;

    if ((rc = close(fd)) < 0)
	unix_error("Close error");
}

int Select(int  n, fd_set *readfds, fd_set *writefds,
	   fd_set *exceptfds, struct timeval *timeout) 
{
    int rc;

    if ((rc = select(n, readfds, writefds, exceptfds, timeout)) < 0)
	unix_error("Select error");
    return rc;
}

int Dup2(int fd1, int fd2) 
{
    int rc;

    if ((rc = dup2(fd1, fd2)) < 0)
	unix_error("Dup2 error");
    return rc;
}

void Stat(const char *filename, struct stat *buf) 
{
    if (stat(filename, buf) < 0)
	unix_error("Stat error");
}

void Fstat(int fd, struct stat *buf) 
{
    if (fstat(fd, buf) < 0)
	unix_error("Fstat error");
}

/*********************************
 * Wrappers for directory function
 *********************************/

DIR *Opendir(const char *name) 
{
    DIR *dirp = opendir(name); 

    if (!dirp)
        unix_error("opendir error");
    return dirp;
}

struct dirent *Readdir(DIR *dirp)
{
    struct dirent *dep;
    
    errno = 0;
    dep = readdir(dirp);
    if ((dep == NULL) && (errno != 0))
        unix_error("readdir error");
    return dep;
}

int Closedir(DIR *dirp) 
{
    int rc;

    if ((rc = closedir(dirp)) < 0)
        unix_error("closedir error");
    return rc;
}

/***************************************
 * Wrappers for memory mapping functions
 ***************************************/
void *Mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) 
{
    void *ptr;

    if ((ptr = mmap(addr, len, prot, flags, fd, offset)) == ((void *) -1))
	unix_error("mmap error");
    return(ptr);
}

void Munmap(void *start, size_t length) 
{
    if (munmap(start, length) < 0)
	unix_error("munmap error");
}

/***************************************************
 * Wrappers for dynamic storage allocation functions
 ***************************************************/

void *Malloc(size_t size) 
{
    void *p;

    if ((p  = malloc(size)) == NULL)
	unix_error("Malloc error");
    return p;
}

void *Realloc(void *ptr, size_t size) 
{
    void *p;

    if ((p  = realloc(ptr, size)) == NULL)
	unix_error("Realloc error");
    return p;
}

void *Calloc(size_t nmemb, size_t size) 
{
    void *p;

    if ((p = calloc(nmemb, size)) == NULL)
	unix_error("Calloc error");
    return p;
}

void Free(void *ptr) 
{
    free(ptr);
}

/******************************************
 * Wrappers for the Standard I/O functions.
 ******************************************/
void Fclose(FILE *fp) 
{
    if (fclose(fp) != 0)
	unix_error("Fclose error");
}

FILE *Fdopen(int fd, const char *type) 
{
    FILE *fp;

    if ((fp = fdopen(fd, type)) == NULL)
	unix_error("Fdopen error");

    return fp;
}

char *Fgets(char *ptr, int n, FILE *stream) 
{
    char *rptr;

    if (((rptr = fgets(ptr, n, stream)) == NULL) && ferror(stream))
	app_error("Fgets error");

    return rptr;
}

FILE *Fopen(const char *filename, const char *mode) 
{
    FILE *fp;

    if ((fp = fopen(filename, mode)) == NULL)
	unix_error("Fopen error");

    return fp;
}

void Fputs(const char *ptr, FILE *stream) 
{
    if (fputs(ptr, stream) == EOF)
	unix_error("Fputs error");
}

size_t Fread(void *ptr, size_t size, size_t nmemb, FILE *stream) 
{
    size_t n;

    if (((n = fread(ptr, size, nmemb, stream)) < nmemb) && ferror(stream)) 
	unix_error("Fread error");
    return n;
}

void Fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) 
{
    if (fwrite(ptr, size, nmemb, stream) < nmemb)
	unix_error("Fwrite error");
}


/**************************** 
 * Sockets interface wrappers
 ****************************/
/*
 * 소켓 인터페이스 래퍼들
 *
 * 소켓은 네트워크 통신을 위한 파일 디스크립터입니다. TCP 연결이 성립된 뒤에는
 * 일반 파일처럼 read/write/Rio_*로 데이터를 주고받을 수 있습니다.
 *
 * 서버 쪽 기본 순서:
 *   1. socket() : 통신용 fd를 만듭니다.
 *   2. bind()   : 그 fd에 "내가 사용할 IP/포트"를 붙입니다.
 *   3. listen() : 그 fd를 접속 요청 대기용 소켓으로 바꿉니다.
 *   4. accept() : 접속한 클라이언트 하나와 통신할 새 fd를 받습니다.
 *
 * 클라이언트 쪽 기본 순서:
 *   1. socket()  : 통신용 fd를 만듭니다.
 *   2. connect() : 서버 IP/포트로 TCP 연결을 겁니다.
 *
 * listenfd와 connfd:
 *   listenfd는 "손님을 받는 접수대"입니다. 계속 accept를 기다립니다.
 *   connfd는 accept가 반환하는 "손님 한 명과 연결된 통신 채널"입니다.
 *   HTTP 요청/응답은 listenfd가 아니라 connfd로 읽고 씁니다.
 */

int Socket(int domain, int type, int protocol) 
{
    int rc;

    /*
     * domain: 주소 체계입니다. AF_INET은 IPv4, AF_INET6은 IPv6입니다.
     * type: 통신 방식입니다. SOCK_STREAM은 TCP처럼 연결 지향 바이트 스트림입니다.
     * protocol: 보통 0을 넣으면 domain/type에 맞는 기본 프로토콜을 고릅니다.
     */
    if ((rc = socket(domain, type, protocol)) < 0)
	unix_error("Socket error");
    return rc;
}

void Setsockopt(int s, int level, int optname, const void *optval, int optlen) 
{
    int rc;

    /*
     * setsockopt는 소켓 옵션을 바꿉니다.
     * 이 파일에서는 open_listenfd에서 SO_REUSEADDR를 켭니다. 서버를 껐다가
     * 바로 다시 켤 때 이전 TCP 연결 흔적 때문에 "Address already in use"가
     * 나는 경우를 줄여줍니다.
     */
    if ((rc = setsockopt(s, level, optname, optval, optlen)) < 0)
	unix_error("Setsockopt error");
}

void Bind(int sockfd, struct sockaddr *my_addr, int addrlen) 
{
    int rc;

    /*
     * bind는 서버가 사용할 주소를 소켓에 붙입니다.
     * 예: "이 소켓은 15213번 포트로 들어오는 연결을 받을 것이다."
     */
    if ((rc = bind(sockfd, my_addr, addrlen)) < 0)
	unix_error("Bind error");
}

void Listen(int s, int backlog) 
{
    int rc;

    /*
     * listen은 bind된 소켓을 접속 요청 대기 상태로 만듭니다.
     * backlog는 아직 accept되지 않은 연결 요청을 커널이 큐에 얼마나 쌓아둘지
     * 알려주는 값입니다.
     */
    if ((rc = listen(s,  backlog)) < 0)
	unix_error("Listen error");
}

int Accept(int s, struct sockaddr *addr, socklen_t *addrlen) 
{
    int rc;

    /*
     * accept는 listening socket에서 클라이언트 연결 하나를 꺼내고,
     * 그 클라이언트와 실제 통신할 새 fd를 반환합니다.
     *
     * 반환값 rc가 바로 connfd입니다. 이후 HTTP 요청을 읽거나 응답을 쓸 때는
     * 이 connfd를 사용합니다.
     */
    if ((rc = accept(s, addr, addrlen)) < 0)
	unix_error("Accept error");
    return rc;
}

void Connect(int sockfd, struct sockaddr *serv_addr, int addrlen) 
{
    int rc;

    /*
     * connect는 클라이언트 입장에서 서버로 TCP 연결을 거는 함수입니다.
     * 성공하면 sockfd는 서버와 연결된 fd가 되고, 이후 read/write가 가능합니다.
     */
    if ((rc = connect(sockfd, serv_addr, addrlen)) < 0)
	unix_error("Connect error");
}

/*******************************
 * Protocol-independent wrappers
 *******************************/
/*
 * 프로토콜 독립 주소 변환 래퍼들
 *
 * 예전 코드에서는 IPv4만 생각하고 sockaddr_in 구조체를 직접 채우는 경우가
 * 많았습니다. 하지만 지금은 IPv4/IPv6를 모두 고려해야 하므로 getaddrinfo를
 * 사용하는 방식이 권장됩니다.
 *
 * getaddrinfo(hostname, port, hints, &listp)는 다음 일을 해줍니다.
 *   - "www.example.com" 같은 이름을 실제 IP 주소 후보로 바꿉니다.
 *   - "80" 같은 포트 문자열을 네트워크 주소 구조체 안에 넣어줍니다.
 *   - IPv4인지 IPv6인지에 맞는 sockaddr 구조체 목록을 만들어줍니다.
 *
 * 결과는 연결 리스트(listp)입니다. 사용자는 그 목록을 돌면서 socket/connect
 * 또는 socket/bind를 시도하면 됩니다.
 */
/* $begin getaddrinfo */
void Getaddrinfo(const char *node, const char *service, 
                 const struct addrinfo *hints, struct addrinfo **res)
{
    int rc;

    /*
     * node: 호스트 이름 또는 IP 문자열입니다. 예: "localhost", "example.com".
     * service: 포트 번호 또는 서비스 이름입니다. 예: "80", "http".
     * hints: 원하는 조건입니다. 예: TCP 소켓, 숫자 포트만 허용.
     * res: 결과 주소 후보 목록의 시작 주소가 저장됩니다.
     */
    if ((rc = getaddrinfo(node, service, hints, res)) != 0) 
        gai_error(rc, "Getaddrinfo error");
}
/* $end getaddrinfo */

void Getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, 
                 size_t hostlen, char *serv, size_t servlen, int flags)
{
    int rc;

    if ((rc = getnameinfo(sa, salen, host, hostlen, serv, 
                          servlen, flags)) != 0) 
        gai_error(rc, "Getnameinfo error");
}

void Freeaddrinfo(struct addrinfo *res)
{
    /* getaddrinfo가 만든 주소 후보 목록은 사용 후 반드시 해제합니다. */
    freeaddrinfo(res);
}

void Inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    /*
     * network to presentation:
     * 커널이 쓰는 바이너리 IP 주소를 사람이 읽는 문자열로 바꿉니다.
     * 예: 4바이트 IPv4 주소 -> "127.0.0.1"
     */
    if (!inet_ntop(af, src, dst, size))
        unix_error("Inet_ntop error");
}

void Inet_pton(int af, const char *src, void *dst) 
{
    int rc;

    /*
     * presentation to network:
     * 사람이 읽는 IP 문자열을 커널이 쓰는 바이너리 주소로 바꿉니다.
     * 예: "127.0.0.1" -> 4바이트 IPv4 주소
     */
    rc = inet_pton(af, src, dst);
    if (rc == 0)
	app_error("inet_pton error: invalid dotted-decimal address");
    else if (rc < 0)
        unix_error("Inet_pton error");
}

/*******************************************
 * DNS interface wrappers. 
 *
 * NOTE: These are obsolete because they are not thread safe. Use
 * getaddrinfo and getnameinfo instead
 ***********************************/

/* $begin gethostbyname */
struct hostent *Gethostbyname(const char *name) 
{
    struct hostent *p;

    if ((p = gethostbyname(name)) == NULL)
	dns_error("Gethostbyname error");
    return p;
}
/* $end gethostbyname */

struct hostent *Gethostbyaddr(const char *addr, int len, int type) 
{
    struct hostent *p;

    if ((p = gethostbyaddr(addr, len, type)) == NULL)
	dns_error("Gethostbyaddr error");
    return p;
}

/************************************************
 * Wrappers for Pthreads thread control functions
 ************************************************/

void Pthread_create(pthread_t *tidp, pthread_attr_t *attrp, 
		    void * (*routine)(void *), void *argp) 
{
    int rc;

    if ((rc = pthread_create(tidp, attrp, routine, argp)) != 0)
	posix_error(rc, "Pthread_create error");
}

void Pthread_cancel(pthread_t tid) {
    int rc;

    if ((rc = pthread_cancel(tid)) != 0)
	posix_error(rc, "Pthread_cancel error");
}

void Pthread_join(pthread_t tid, void **thread_return) {
    int rc;

    if ((rc = pthread_join(tid, thread_return)) != 0)
	posix_error(rc, "Pthread_join error");
}

/* $begin detach */
void Pthread_detach(pthread_t tid) {
    int rc;

    if ((rc = pthread_detach(tid)) != 0)
	posix_error(rc, "Pthread_detach error");
}
/* $end detach */

void Pthread_exit(void *retval) {
    pthread_exit(retval);
}

pthread_t Pthread_self(void) {
    return pthread_self();
}
 
void Pthread_once(pthread_once_t *once_control, void (*init_function)()) {
    pthread_once(once_control, init_function);
}

/*******************************
 * Wrappers for Posix semaphores
 *******************************/

void Sem_init(sem_t *sem, int pshared, unsigned int value) 
{
    if (sem_init(sem, pshared, value) < 0)
	unix_error("Sem_init error");
}

void P(sem_t *sem) 
{
    if (sem_wait(sem) < 0)
	unix_error("P error");
}

void V(sem_t *sem) 
{
    if (sem_post(sem) < 0)
	unix_error("V error");
}

/****************************************
 * The Rio package - Robust I/O functions
 ****************************************/
/*
 * Rio(Robust I/O) 패키지
 *
 * read/write는 "요청한 바이트 수를 항상 한 번에 처리한다"는 보장이 없습니다.
 * 예를 들어 100바이트를 읽으라고 했는데 커널 버퍼에 17바이트만 준비되어 있으면
 * read는 17만 반환할 수 있습니다. write도 일부만 쓰고 돌아올 수 있습니다.
 *
 * Rio는 이런 partial read/write를 반복 호출로 보완합니다.
 *
 * 웹 프록시에서 중요한 이유:
 *   - HTTP 헤더는 줄 단위 텍스트라 Rio_readlineb가 편합니다.
 *   - HTTP 본문이나 이미지 같은 데이터는 바이트 단위로 정확히 옮겨야 합니다.
 *   - 네트워크 I/O는 시그널에 의해 중간에 끊길 수 있으므로 EINTR 처리도 필요합니다.
 *
 * buffered와 unbuffered:
 *   - rio_readn/rio_writen: 내부 버퍼 없이 fd에서 바로 n바이트 처리
 *   - rio_readinitb + rio_readlineb/rio_readnb: 내부 버퍼를 사용해 효율적으로 읽기
 */

/*
 * rio_readn - Robustly read n bytes (unbuffered)
 */
/* $begin rio_readn */
ssize_t rio_readn(int fd, void *usrbuf, size_t n) 
{
    size_t nleft = n;
    ssize_t nread;
    char *bufp = usrbuf;

    /*
     * 목표: fd에서 정확히 n바이트를 읽으려고 반복합니다.
     * EOF를 만나면 n보다 적게 읽고 끝날 수 있습니다.
     */
    while (nleft > 0) {
	if ((nread = read(fd, bufp, nleft)) < 0) {
	    if (errno == EINTR) /* Interrupted by sig handler return */
		nread = 0;      /* and call read() again */
	    else
		return -1;      /* errno set by read() */ 
	} 
	else if (nread == 0)
	    break;              /* EOF */
	/*
	 * read가 일부만 읽었을 수 있으므로, 남은 바이트 수와 다음 저장 위치를
	 * 갱신하고 계속 반복합니다.
	 */
	nleft -= nread;
	bufp += nread;
    }
    return (n - nleft);         /* Return >= 0 */
}
/* $end rio_readn */

/*
 * rio_writen - Robustly write n bytes (unbuffered)
 */
/* $begin rio_writen */
ssize_t rio_writen(int fd, void *usrbuf, size_t n) 
{
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;

    /*
     * 목표: usrbuf의 n바이트가 전부 fd로 나갈 때까지 write를 반복합니다.
     * 소켓으로 HTTP 요청/응답을 보낼 때 일부만 전송되는 문제를 막아줍니다.
     */
    while (nleft > 0) {
	if ((nwritten = write(fd, bufp, nleft)) <= 0) {
	    if (errno == EINTR)  /* Interrupted by sig handler return */
		nwritten = 0;    /* and call write() again */
	    else
		return -1;       /* errno set by write() */
	}
	/* 방금 쓴 만큼 건너뛰고, 아직 못 쓴 나머지를 계속 씁니다. */
	nleft -= nwritten;
	bufp += nwritten;
    }
    return n;
}
/* $end rio_writen */


/* 
 * rio_read - This is a wrapper for the Unix read() function that
 *    transfers min(n, rio_cnt) bytes from an internal buffer to a user
 *    buffer, where n is the number of bytes requested by the user and
 *    rio_cnt is the number of unread bytes in the internal buffer. On
 *    entry, rio_read() refills the internal buffer via a call to
 *    read() if the internal buffer is empty.
 */
/* $begin rio_read */
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n)
{
    int cnt;

    while (rp->rio_cnt <= 0) {  /* Refill if buf is empty */
	/*
	 * 내부 버퍼가 비어 있으면 커널 fd에서 RIO_BUFSIZE만큼 한 번에 가져옵니다.
	 * 사용자가 1바이트씩 요청하더라도 매번 read 시스템 콜을 하지 않게 하려는
	 * 목적입니다.
	 */
	rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, 
			   sizeof(rp->rio_buf));
	if (rp->rio_cnt < 0) {
	    if (errno != EINTR) /* Interrupted by sig handler return */
		return -1;
	}
	else if (rp->rio_cnt == 0)  /* EOF */
	    return 0;
	else 
	    rp->rio_bufptr = rp->rio_buf; /* Reset buffer ptr */
    }

    /* Copy min(n, rp->rio_cnt) bytes from internal buf to user buf */
    /*
     * 내부 버퍼에 남아 있는 양과 사용자가 요청한 양 중 더 작은 만큼만 복사합니다.
     * 복사 후에는 포인터와 남은 개수를 갱신해서 다음 호출이 이어서 읽게 합니다.
     */
    cnt = n;          
    if (rp->rio_cnt < n)   
	cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}
/* $end rio_read */

/*
 * rio_readinitb - Associate a descriptor with a read buffer and reset buffer
 */
/* $begin rio_readinitb */
void rio_readinitb(rio_t *rp, int fd) 
{
    /*
     * rio_t 구조체를 특정 fd와 연결하고 내부 버퍼 상태를 초기화합니다.
     * Rio_readlineb/Rio_readnb를 쓰기 전에 반드시 한 번 호출해야 합니다.
     */
    rp->rio_fd = fd;  
    rp->rio_cnt = 0;  
    rp->rio_bufptr = rp->rio_buf;
}
/* $end rio_readinitb */

/*
 * rio_readnb - Robustly read n bytes (buffered)
 */
/* $begin rio_readnb */
ssize_t rio_readnb(rio_t *rp, void *usrbuf, size_t n) 
{
    size_t nleft = n;
    ssize_t nread;
    char *bufp = usrbuf;
    
    /*
     * buffered 버전의 rio_readn입니다. 내부 버퍼(rp)를 사용한다는 점이 다릅니다.
     * HTTP body처럼 "정해진 바이트 수만큼" 읽을 때 사용할 수 있습니다.
     */
    while (nleft > 0) {
	if ((nread = rio_read(rp, bufp, nleft)) < 0) 
            return -1;          /* errno set by read() */ 
	else if (nread == 0)
	    break;              /* EOF */
	nleft -= nread;
	bufp += nread;
    }
    return (n - nleft);         /* return >= 0 */
}
/* $end rio_readnb */

/* 
 * rio_readlineb - Robustly read a text line (buffered)
 */
/* $begin rio_readlineb */
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen) 
{
    int n, rc;
    char c, *bufp = usrbuf;

    /*
     * 한 줄을 읽습니다. '\n'을 만날 때까지 1바이트씩 가져오지만, 실제 fd read는
     * rio_read 내부 버퍼 덕분에 덩어리 단위로 일어납니다.
     *
     * HTTP 요청/응답 헤더는 줄 단위입니다. 예를 들어:
     *   GET /index.html HTTP/1.0\r\n
     *   Host: example.com\r\n
     *   \r\n
     * 이런 헤더를 읽을 때 Rio_readlineb가 매우 유용합니다.
     */
    for (n = 1; n < maxlen; n++) { 
        if ((rc = rio_read(rp, &c, 1)) == 1) {
            *bufp++ = c;
            if (c == '\n') {
                n++;
                break;
            }
        } else if (rc == 0) {
            if (n == 1)
                return 0; /* EOF, no data read */
            else
                break;    /* EOF, some data was read */
        } else
            return -1;	  /* Error */
    }
    *bufp = 0;
    return n-1;
}
/* $end rio_readlineb */

/**********************************
 * Wrappers for robust I/O routines
 **********************************/
ssize_t Rio_readn(int fd, void *ptr, size_t nbytes) 
{
    ssize_t n;
  
    if ((n = rio_readn(fd, ptr, nbytes)) < 0)
	unix_error("Rio_readn error");
    return n;
}

void Rio_writen(int fd, void *usrbuf, size_t n) 
{
    if (rio_writen(fd, usrbuf, n) != n)
	unix_error("Rio_writen error");
}

void Rio_readinitb(rio_t *rp, int fd)
{
    rio_readinitb(rp, fd);
} 

ssize_t Rio_readnb(rio_t *rp, void *usrbuf, size_t n) 
{
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0)
	unix_error("Rio_readnb error");
    return rc;
}

ssize_t Rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen) 
{
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0)
	unix_error("Rio_readlineb error");
    return rc;
} 

/******************************** 
 * Client/server helper functions
 ********************************/
/*
 * open_clientfd - Open connection to server at <hostname, port> and
 *     return a socket descriptor ready for reading and writing. This
 *     function is reentrant and protocol-independent.
 *
 *     On error, returns: 
 *       -2 for getaddrinfo error
 *       -1 with errno set for other errors.
 */
/* $begin open_clientfd */
int open_clientfd(char *hostname, char *port) {
    int clientfd, rc;
    struct addrinfo hints, *listp, *p;

    /* Get a list of potential server addresses */
    /*
     * 클라이언트용 연결 헬퍼입니다.
     *
     * 입력:
     *   hostname: 접속할 서버 이름입니다. 예: "example.com", "localhost"
     *   port: 접속할 서버 포트입니다. 예: "80", "8080"
     *
     * 성공하면 서버와 이미 connect된 소켓 fd를 반환합니다.
     * 즉, 반환된 clientfd에는 바로 Rio_writen/Rio_readlineb 등을 사용할 수 있습니다.
     *
     * 프록시 관점:
     *   브라우저가 프록시에 요청한 URL에서 host/port를 뽑고,
     *   프록시가 실제 웹 서버로 나가는 연결을 만들 때 이 함수를 씁니다.
     */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;  /* TCP 연결을 원한다는 뜻입니다. */
    hints.ai_flags = AI_NUMERICSERV;  /* port가 "http"가 아니라 "80" 같은 숫자 문자열이라고 가정합니다. */
    hints.ai_flags |= AI_ADDRCONFIG;  /* 현재 머신에서 사용 가능한 주소 체계(IPv4/IPv6)를 우선 고려합니다. */
    if ((rc = getaddrinfo(hostname, port, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo failed (%s:%s): %s\n", hostname, port, gai_strerror(rc));
        return -2;
    }
  
    /* Walk the list for one that we can successfully connect to */
    /*
     * getaddrinfo는 주소 후보를 여러 개 줄 수 있습니다.
     * 예: IPv6 후보, IPv4 후보, 도메인 하나에 연결된 여러 IP.
     * 그중 하나라도 socket 생성 + connect에 성공하면 연결 성공입니다.
     */

    //p조건은 p != NULL과 완전 동일
    //NULL은 0이랑 동일하기 때문
    for (p = listp; p; p = p->ai_next) {
        /* Create a socket descriptor */
        /*
         * p가 가진 주소 체계(ai_family), 소켓 타입(ai_socktype), 프로토콜(ai_protocol)에
         * 맞춰 소켓 fd를 만듭니다.
         */
        if ((clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) 
            continue; /* Socket failed, try the next */

        /* Connect to the server */
        /*
         * TCP 연결을 시도합니다. 성공하면 clientfd는 서버와 연결된 소켓이 됩니다.
         */
        if (connect(clientfd, p->ai_addr, p->ai_addrlen) != -1) 
            break; /* Success */
        /*
         * 이 주소 후보로는 연결이 안 됐으므로 fd를 닫고 다음 후보를 시도합니다.
         * 실패한 fd를 닫지 않으면 fd 누수가 생깁니다.
         */
        if (close(clientfd) < 0) { /* Connect failed, try another */  //line:netp:openclientfd:closefd
            fprintf(stderr, "open_clientfd: close failed: %s\n", strerror(errno));
            return -1;
        } 
    } 

    /* Clean up */
    freeaddrinfo(listp);
    if (!p) /* All connects failed */
        return -1;
    else    /* The last connect succeeded */
        return clientfd;
}
/* $end open_clientfd */

/*  
 * open_listenfd - Open and return a listening socket on port. This
 *     function is reentrant and protocol-independent.
 *
 *     On error, returns: 
 *       -2 for getaddrinfo error
 *       -1 with errno set for other errors.
 */
/* $begin open_listenfd */
int open_listenfd(char *port) 
{
    struct addrinfo hints, *listp, *p;
    int listenfd, rc, optval=1;

    /* Get a list of potential server addresses */
    /*
     * 서버용 listen 소켓 헬퍼입니다.
     *
     * 입력:
     *   port: 서버가 기다릴 포트 번호 문자열입니다. 예: "15213"
     *
     * 성공하면 accept를 호출할 수 있는 listening socket fd를 반환합니다.
     *
     * 프록시 관점:
     *   브라우저가 프록시에 접속할 수 있도록 프록시가 자기 포트를 열 때 씁니다.
     */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;             /* TCP 연결을 받을 소켓을 원합니다. */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* NULL host와 함께 쓰면 "내 모든 적절한 IP"에서 받겠다는 뜻입니다. */
    hints.ai_flags |= AI_NUMERICSERV;            /* port가 숫자 문자열이라고 알려줍니다. */
    if ((rc = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo failed (port %s): %s\n", port, gai_strerror(rc));
        return -2;
    }

    /* Walk the list for one that we can bind to */
    /*
     * 여러 주소 후보 중 bind에 성공하는 하나를 찾습니다.
     * 환경에 따라 IPv6 후보가 먼저 나오고 실패한 뒤 IPv4에서 성공할 수도 있습니다.
     */
    for (p = listp; p; p = p->ai_next) {
        /* Create a socket descriptor */
        if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) 
            continue;  /* Socket failed, try the next */

        /* Eliminates "Address already in use" error from bind */
        /*
         * TCP 연결을 끊으면, ESTABLISHED → FIN_WAIT → TIME_WAIT 단계를 거친다.
         * TIME_WAIT은 몇십 초 유지되는데, 이 때 다시 서버를 키면, bind에 실패한다.
         * 아래는 TIME_WAIT 상태여도, 이 포트 다시 써도 된다고 설정하는 함수
         */
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,    //line:netp:csapp:setsockopt
                   (const void *)&optval , sizeof(int));

        /* Bind the descriptor to the address */
        /*
         * 성공하면 이 listenfd는 port에 묶인 상태가 됩니다.
         * 아직 클라이언트를 받는 상태는 아니고, 아래 listen 호출까지 해야 합니다.
         */
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break; /* Success */
        
        //bind에 실패한 파일디스크립터는 close()로 닫는다.
        if (close(listenfd) < 0) { /* Bind failed, try the next */
            fprintf(stderr, "open_listenfd close failed: %s\n", strerror(errno));
            return -1;
        }
    }


    /* Clean up */
    freeaddrinfo(listp);
    if (!p) /* No address worked */
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    /*
     * bind된 fd를 listening socket으로 전환합니다.
     * 이 함수가 성공해야 main 루프에서 Accept(listenfd, ...)를 호출할 수 있습니다.
     */
    if (listen(listenfd, LISTENQ) < 0) {
        close(listenfd);
	return -1;
    }
    return listenfd;
}
/* $end open_listenfd */

/****************************************************
 * Wrappers for reentrant protocol-independent helpers
 ****************************************************/
int Open_clientfd(char *hostname, char *port) 
{
    int rc;

    if ((rc = open_clientfd(hostname, port)) < 0) 
	unix_error("Open_clientfd error");
    return rc;
}

int Open_listenfd(char *port) 
{
    int rc;

    if ((rc = open_listenfd(port)) < 0)
	unix_error("Open_listenfd error");
    return rc;
}

/* $end csapp.c */
