#include "csapp.h"

#include <errno.h>
#include <limits.h>
#include <strings.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/*
 * 캐시를 "정확히 몇 개의 슬롯"으로 둘지는 과제에서 정해주지 않습니다.
 * 객체 하나는 최대 100KiB이고, 총 캐시 크기는 약 1MiB이므로 20개 슬롯이면
 * 일반적인 테스트를 충분히 담을 수 있습니다.
 *
 * 실제 제한은 slot 개수가 아니라 total_size로 관리하므로,
 * 저장된 실제 객체 바이트 수가 MAX_CACHE_SIZE를 넘지 않도록 합니다.
 */
#define CACHE_BLOCK_COUNT 20

/* HTTP 요청을 다시 만들 때 항상 넣어야 하는 헤더들 */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";

typedef struct {
  char uri[MAXLINE];
  char object[MAX_OBJECT_SIZE];
  int size;
  int valid;
  unsigned long lru;

  /*
   * 각 캐시 블록은 pthread readers-writer lock 하나로 보호합니다.
   *
   * read lock:
   *   여러 스레드가 같은 캐시 객체를 동시에 읽을 수 있습니다.
   * write lock:
   *   삽입/축출/LRU 갱신처럼 메타데이터나 객체 내용을 바꿀 때 사용합니다.
   */
  pthread_rwlock_t lock;
} cache_block_t;

typedef struct {
  cache_block_t blocks[CACHE_BLOCK_COUNT];
  size_t total_size;

  /*
   * cache_write_mutex:
   *   "캐시에 쓰는 작업"은 한 번에 하나만 하도록 보장합니다.
   *   삽입/축출뿐 아니라 LRU 시간 갱신도 이 락 아래에서 수행합니다.
   *
   * time_mutex:
   *   전역 사용 시각(use clock)을 증가시킬 때 사용합니다.
   */
  pthread_mutex_t cache_write_mutex;
  pthread_mutex_t time_mutex;
  unsigned long use_clock;
} cache_t;

typedef struct {
  int connfd;
  struct sockaddr_storage clientaddr;
} thread_args_t;

static cache_t cache;

void *thread(void *vargp);
void doit(int connfd);

void clienterror(int fd, const char *cause, const char *errnum,
                 const char *shortmsg, const char *longmsg);

int parse_uri(const char *uri, char *hostname, char *path, char *port);
int parse_host_header_value(const char *host_value, char *hostname, char *port);
void trim_line_end(char *s);
int append_header(char *dst, size_t dst_size, const char *line);
void build_http_request(char *http_request, size_t request_size,
                        const char *path, const char *host_header,
                        const char *other_headers);

void cache_init(void);
unsigned long cache_next_timestamp(void);
void cache_block_read_lock(cache_block_t *block);
void cache_block_read_unlock(cache_block_t *block);
void cache_block_write_lock(cache_block_t *block);
void cache_block_write_unlock(cache_block_t *block);
int cache_find_and_send(int connfd, const char *uri);
void cache_mark_used(int index, const char *uri);
void cache_store(const char *uri, const char *buf, int size);
int cache_find_empty_slot(void);
int cache_find_lru_slot(void);
void cache_evict_if_needed(size_t required_size);

int main(int argc, char **argv) {
  int listenfd;
  socklen_t clientlen;
  pthread_t tid;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  /*
   * 브라우저가 먼저 연결을 끊은 뒤 proxy가 write를 시도하면 SIGPIPE가 날 수 있습니다.
   * 기본 동작은 프로세스 종료라서, long-running 서버에는 치명적입니다.
   * 따라서 SIGPIPE는 무시하고 write/Rio_writen의 실패를 일반 에러처럼 처리합니다.
   */
  Signal(SIGPIPE, SIG_IGN);

  cache_init();
  listenfd = Open_listenfd(argv[1]);

  while (1) {
    thread_args_t *args = Malloc(sizeof(thread_args_t));

    clientlen = sizeof(args->clientaddr);
    args->connfd = Accept(listenfd, (SA *)&args->clientaddr, &clientlen);

    /*
     * 연결 하나당 detached thread 하나를 생성합니다.
     * detached 모드로 만들면 join하지 않아도 스레드 자원이 자동 회수되어
     * 과제 문서가 요구한 메모리 누수 없는 concurrent server가 됩니다.
     */
    Pthread_create(&tid, NULL, thread, args);
  }
}

void *thread(void *vargp) {
  thread_args_t *args = (thread_args_t *)vargp;
  int connfd = args->connfd;

  Pthread_detach(pthread_self());
  Free(args);

  doit(connfd);
  Close(connfd);
  return NULL;
}

void doit(int connfd) {
  int serverfd;
  int n;
  rio_t client_rio;
  rio_t server_rio;

  char buf[MAXLINE];
  char method[MAXLINE];
  char uri[MAXLINE];
  char version[MAXLINE];

  char hostname[MAXLINE];
  char path[MAXLINE];
  char port[16];

  char host_header_value[MAXLINE];
  char generated_host_header[MAXLINE];
  char other_headers[MAXBUF * 4];
  char http_request[MAXBUF * 8];
  char cache_buf[MAX_OBJECT_SIZE];
  int total_size;
  int object_cacheable;

  Rio_readinitb(&client_rio, connfd);

  /*
   * 빈 연결(예: 브라우저가 즉시 끊은 경우)은 그냥 조용히 종료합니다.
   * 서버 전체가 죽으면 안 되므로, 연결 단위 에러로 다룹니다.
   */
  if ((n = Rio_readlineb(&client_rio, buf, MAXLINE)) <= 0) {
    return;
  }

  if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
    clienterror(connfd, buf, "400", "Bad Request",
                "Proxy could not parse the request line");
    return;
  }

  if (strcasecmp(method, "GET")) {
    clienterror(connfd, method, "501", "Not Implemented",
                "Proxy only implements the GET method");
    return;
  }

  if (!parse_uri(uri, hostname, path, port)) {
    clienterror(connfd, uri, "400", "Bad Request",
                "Proxy could not parse the URI");
    return;
  }

  host_header_value[0] = '\0';
  other_headers[0] = '\0';

  /*
   * 요청 헤더를 끝까지 읽으면서:
   * 1) Host 헤더는 따로 저장
   * 2) User-Agent / Connection / Proxy-Connection 은 우리가 다시 넣을 것이므로 건너뜀
   * 3) 그 외 헤더는 그대로 forwarding
   */
  while ((n = Rio_readlineb(&client_rio, buf, MAXLINE)) > 0) {
    if (!strcmp(buf, "\r\n")) {
      break;
    }

    if (!strncasecmp(buf, "Host:", 5)) {
      char *value = buf + 5;
      while (*value == ' ' || *value == '\t') {
        value++;
      }
      strncpy(host_header_value, value, sizeof(host_header_value) - 1);
      host_header_value[sizeof(host_header_value) - 1] = '\0';
      trim_line_end(host_header_value);
      continue;
    }

    if (!strncasecmp(buf, "User-Agent:", 11) ||
        !strncasecmp(buf, "Connection:", 11) ||
        !strncasecmp(buf, "Proxy-Connection:", 17)) {
      continue;
    }

    if (!append_header(other_headers, sizeof(other_headers), buf)) {
      clienterror(connfd, "", "400", "Bad Request",
                  "Request headers are too large");
      return;
    }
  }

  /*
   * absolute-form URI에 host가 없고, 대신 Host 헤더로만 서버를 식별할 수도 있습니다.
   * 그런 경우 Host 헤더를 다시 파싱해서 실제 목적지 host/port를 채웁니다.
   */
  if (hostname[0] == '\0') {
    if (host_header_value[0] == '\0' ||
        !parse_host_header_value(host_header_value, hostname, port)) {
      clienterror(connfd, "", "400", "Bad Request",
                  "Request is missing a valid host");
      return;
    }
  }

  /*
   * 같은 객체인지 판단할 때는 "어느 서버의 어느 포트에서 어떤 path를 요청했는가"를
   * 모두 포함해야 합니다. 그래야 example.com:80/index.html 과
   * example.com:8080/index.html 을 서로 다른 객체로 구분할 수 있습니다.
   */
  snprintf(generated_host_header, sizeof(generated_host_header), "%s", hostname);
  if (host_header_value[0] == '\0' && strcmp(port, "80")) {
    snprintf(generated_host_header, sizeof(generated_host_header), "%s:%s",
             hostname, port);
  }

  snprintf(uri, sizeof(uri), "%s:%s%s", hostname, port, path);

  if (cache_find_and_send(connfd, uri)) {
    return;
  }

  build_http_request(http_request, sizeof(http_request), path,
                     host_header_value[0] ? host_header_value
                                          : generated_host_header,
                     other_headers);

  if ((serverfd = open_clientfd(hostname, port)) < 0) {
    clienterror(connfd, hostname, "502", "Bad Gateway",
                "Proxy could not connect to the end server");
    return;
  }

  Rio_writen(serverfd, http_request, strlen(http_request));

  Rio_readinitb(&server_rio, serverfd);

  total_size = 0;
  object_cacheable = 1;

  /*
   * 응답은 텍스트/바이너리 구분 없이 그대로 client에게 전달해야 하므로,
   * line 단위가 아니라 byte stream 단위로 읽고 씁니다.
   *
   * 동시에, 객체가 MAX_OBJECT_SIZE 이하인 동안에는 cache_buf에 누적해 두었다가
   * 끝까지 다 읽는 데 성공하면 캐시에 넣습니다.
   */
  while ((n = Rio_readnb(&server_rio, buf, MAXBUF)) > 0) {
    Rio_writen(connfd, buf, n);

    if (object_cacheable) {
      if (total_size + n <= MAX_OBJECT_SIZE) {
        memcpy(cache_buf + total_size, buf, n);
      } else {
        object_cacheable = 0;
      }
    }

    total_size += n;
  }

  Close(serverfd);

  if (object_cacheable && total_size <= MAX_OBJECT_SIZE) {
    cache_store(uri, cache_buf, total_size);
  }
}

/*
 * URI 파서의 목표는 다음 3가지를 뽑는 것입니다.
 * 1) hostname
 * 2) path
 * 3) port (없으면 기본값 80)
 *
 * 브라우저가 proxy에게 보내는 요청은 보통 absolute-form:
 *   GET http://host:port/path HTTP/1.1
 *
 * 하지만 테스트/수동입력에서는 origin-form:
 *   GET /path HTTP/1.1
 * 와 Host 헤더 조합도 들어올 수 있으므로, 그 경우 hostname을 빈 문자열로 남겨 두고
 * 뒤에서 Host 헤더로 보완합니다.
 */
int parse_uri(const char *uri, char *hostname, char *path, char *port) {
  const char *hostbegin;
  const char *pathbegin;
  const char *portbegin;
  char hostbuf[MAXLINE];
  size_t hostlen;

  hostname[0] = '\0';
  path[0] = '\0';
  snprintf(port, 16, "80");

  if (!strncasecmp(uri, "http://", 7)) {
    hostbegin = uri + 7;
  } else if (uri[0] == '/') {
    snprintf(path, MAXLINE, "%s", uri);
    return 1;
  } else {
    return 0;
  }

  pathbegin = strchr(hostbegin, '/');
  if (pathbegin) {
    snprintf(path, MAXLINE, "%s", pathbegin);
    hostlen = (size_t)(pathbegin - hostbegin);
  } else {
    snprintf(path, MAXLINE, "/");
    hostlen = strlen(hostbegin);
  }

  if (hostlen == 0 || hostlen >= sizeof(hostbuf)) {
    return 0;
  }

  memcpy(hostbuf, hostbegin, hostlen);
  hostbuf[hostlen] = '\0';

  portbegin = strchr(hostbuf, ':');
  if (portbegin) {
    size_t name_len = (size_t)(portbegin - hostbuf);
    if (name_len == 0 || name_len >= MAXLINE) {
      return 0;
    }

    memcpy(hostname, hostbuf, name_len);
    hostname[name_len] = '\0';
    snprintf(port, 16, "%s", portbegin + 1);
    if (port[0] == '\0') {
      return 0;
    }
  } else {
    snprintf(hostname, MAXLINE, "%s", hostbuf);
  }

  return 1;
}

/*
 * Host 헤더 값 예시:
 *   "localhost"
 *   "localhost:15213"
 */
int parse_host_header_value(const char *host_value, char *hostname, char *port) {
  const char *colon;
  size_t hostlen;

  snprintf(port, 16, "80");

  colon = strchr(host_value, ':');
  if (colon) {
    hostlen = (size_t)(colon - host_value);
    if (hostlen == 0 || hostlen >= MAXLINE) {
      return 0;
    }

    memcpy(hostname, host_value, hostlen);
    hostname[hostlen] = '\0';
    snprintf(port, 16, "%s", colon + 1);
    if (port[0] == '\0') {
      return 0;
    }
  } else {
    if (host_value[0] == '\0') {
      return 0;
    }
    snprintf(hostname, MAXLINE, "%s", host_value);
  }

  return 1;
}

void trim_line_end(char *s) {
  size_t len = strlen(s);

  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[len - 1] = '\0';
    len--;
  }
}

int append_header(char *dst, size_t dst_size, const char *line) {
  size_t dst_len = strlen(dst);
  size_t line_len = strlen(line);

  if (dst_len + line_len + 1 > dst_size) {
    return 0;
  }

  memcpy(dst + dst_len, line, line_len + 1);
  return 1;
}

void build_http_request(char *http_request, size_t request_size,
                        const char *path, const char *host_header,
                        const char *other_headers) {
  /*
   * proxy가 서버로 보내는 요청은 항상 HTTP/1.0 으로 내립니다.
   * 그리고 과제에서 요구한 필수 헤더들을 명시적으로 다시 구성합니다.
   */
  snprintf(http_request, request_size,
           "GET %s HTTP/1.0\r\n"
           "Host: %s\r\n"
           "%s"
           "%s"
           "%s"
           "%s"
           "\r\n",
           path, host_header, user_agent_hdr, connection_hdr,
           proxy_connection_hdr, other_headers);
}

void clienterror(int fd, const char *cause, const char *errnum,
                 const char *shortmsg, const char *longmsg) {
  char buf[MAXLINE];
  char body[MAXBUF];

  snprintf(body, sizeof(body),
           "<html><title>Proxy Error</title>"
           "<body bgcolor=\"ffffff\">"
           "%s: %s<br>"
           "%s: %s"
           "<hr><em>Proxy Web server</em>"
           "</body></html>",
           errnum, shortmsg, longmsg, cause);

  snprintf(buf, sizeof(buf),
           "HTTP/1.0 %s %s\r\n"
           "Content-type: text/html\r\n"
           "Content-length: %zu\r\n\r\n",
           errnum, shortmsg, strlen(body));

  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}

void cache_init(void) {
  int i;

  cache.total_size = 0;
  cache.use_clock = 0;

  pthread_mutex_init(&cache.cache_write_mutex, NULL);
  pthread_mutex_init(&cache.time_mutex, NULL);

  for (i = 0; i < CACHE_BLOCK_COUNT; i++) {
    cache.blocks[i].uri[0] = '\0';
    cache.blocks[i].size = 0;
    cache.blocks[i].valid = 0;
    cache.blocks[i].lru = 0;
    pthread_rwlock_init(&cache.blocks[i].lock, NULL);
  }
}

unsigned long cache_next_timestamp(void) {
  unsigned long ts;

  pthread_mutex_lock(&cache.time_mutex);
  ts = ++cache.use_clock;
  pthread_mutex_unlock(&cache.time_mutex);
  return ts;
}

void cache_block_read_lock(cache_block_t *block) {
  pthread_rwlock_rdlock(&block->lock);
}

void cache_block_read_unlock(cache_block_t *block) {
  pthread_rwlock_unlock(&block->lock);
}

void cache_block_write_lock(cache_block_t *block) {
  pthread_rwlock_wrlock(&block->lock);
}

void cache_block_write_unlock(cache_block_t *block) {
  pthread_rwlock_unlock(&block->lock);
}

/*
 * 캐시 hit 시에는 서버에 가지 않고 바로 client로 전송합니다.
 * 읽는 동안 block에 대해 read lock을 잡고 있으므로 여러 스레드가 동시에 같은 객체를
 * 읽는 것은 허용되지만, 누군가 그 블록을 바꾸는 것은 막을 수 있습니다.
 */
int cache_find_and_send(int connfd, const char *uri) {
  int i;

  for (i = 0; i < CACHE_BLOCK_COUNT; i++) {
    cache_block_t *block = &cache.blocks[i];

    cache_block_read_lock(block);
    if (block->valid && !strcmp(block->uri, uri)) {
      Rio_writen(connfd, block->object, block->size);
      cache_block_read_unlock(block);

      /*
       * 읽기도 "최근 사용"으로 간주해야 하므로 LRU 타임스탬프를 갱신합니다.
       * 데이터 전송 자체는 read lock 하에서 끝냈고, 그 뒤에 metadata만 write합니다.
       */
      cache_mark_used(i, uri);
      return 1;
    }
    cache_block_read_unlock(block);
  }

  return 0;
}

void cache_mark_used(int index, const char *uri) {
  cache_block_t *block = &cache.blocks[index];

  pthread_mutex_lock(&cache.cache_write_mutex);
  cache_block_write_lock(block);

  if (block->valid && !strcmp(block->uri, uri)) {
    block->lru = cache_next_timestamp();
  }

  cache_block_write_unlock(block);
  pthread_mutex_unlock(&cache.cache_write_mutex);
}

int cache_find_empty_slot(void) {
  int i;

  for (i = 0; i < CACHE_BLOCK_COUNT; i++) {
    int valid;

    cache_block_read_lock(&cache.blocks[i]);
    valid = cache.blocks[i].valid;
    cache_block_read_unlock(&cache.blocks[i]);

    if (!valid) {
      return i;
    }
  }

  return -1;
}

int cache_find_lru_slot(void) {
  int i;
  int victim = -1;
  unsigned long min_lru = ULONG_MAX;

  for (i = 0; i < CACHE_BLOCK_COUNT; i++) {
    int valid;
    unsigned long lru;

    cache_block_read_lock(&cache.blocks[i]);
    valid = cache.blocks[i].valid;
    lru = cache.blocks[i].lru;
    cache_block_read_unlock(&cache.blocks[i]);

    if (valid && lru < min_lru) {
      min_lru = lru;
      victim = i;
    }
  }

  return victim;
}

void cache_evict_if_needed(size_t required_size) {
  while (cache.total_size + required_size > MAX_CACHE_SIZE) {
    int victim = cache_find_lru_slot();
    cache_block_t *block;

    if (victim < 0) {
      return;
    }

    block = &cache.blocks[victim];
    cache_block_write_lock(block);

    if (block->valid) {
      cache.total_size -= block->size;
      block->valid = 0;
      block->size = 0;
      block->uri[0] = '\0';
      block->lru = 0;
    }

    cache_block_write_unlock(block);
  }
}

void cache_store(const char *uri, const char *buf, int size) {
  int i;
  int target;
  cache_block_t *block;

  if (size > MAX_OBJECT_SIZE) {
    return;
  }

  pthread_mutex_lock(&cache.cache_write_mutex);

  /*
   * 이미 같은 객체가 캐시에 있으면 새로 받아온 응답으로 덮어써서 최신 사용 시각을 반영합니다.
   * (엄밀한 HTTP 캐시 일관성 모델은 과제 범위를 넘으므로 단순 overwrite 전략이면 충분합니다.)
   */
  for (i = 0; i < CACHE_BLOCK_COUNT; i++) {
    cache_block_read_lock(&cache.blocks[i]);
    if (cache.blocks[i].valid && !strcmp(cache.blocks[i].uri, uri)) {
      cache_block_read_unlock(&cache.blocks[i]);

      block = &cache.blocks[i];
      cache_block_write_lock(block);
      if (block->valid && !strcmp(block->uri, uri)) {
        cache.total_size -= block->size;
        memcpy(block->object, buf, size);
        snprintf(block->uri, sizeof(block->uri), "%s", uri);
        block->size = size;
        block->valid = 1;
        block->lru = cache_next_timestamp();
        cache.total_size += size;
      }
      cache_block_write_unlock(block);

      cache_evict_if_needed(0);
      pthread_mutex_unlock(&cache.cache_write_mutex);
      return;
    }
    cache_block_read_unlock(&cache.blocks[i]);
  }

  cache_evict_if_needed((size_t)size);

  target = cache_find_empty_slot();
  if (target < 0) {
    target = cache_find_lru_slot();
  }
  if (target < 0) {
    pthread_mutex_unlock(&cache.cache_write_mutex);
    return;
  }

  block = &cache.blocks[target];
  cache_block_write_lock(block);

  if (block->valid) {
    cache.total_size -= block->size;
  }

  memcpy(block->object, buf, size);
  snprintf(block->uri, sizeof(block->uri), "%s", uri);
  block->size = size;
  block->valid = 1;
  block->lru = cache_next_timestamp();
  cache.total_size += size;

  cache_block_write_unlock(block);
  pthread_mutex_unlock(&cache.cache_write_mutex);
}
