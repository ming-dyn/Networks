#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>

#define REQUEST_BUFFER_SIZE 1000
#define EACH_HEADER_SIZE 200
#define RESPONSE_MSG_SIZE 10000000
#define DEFAULT_CACHE_AGE 3600
#define CACHE_CAP 20
#define REQUEST_BUFFER_TIMEOUT_SECONDS 60

/* struct of the buffer */
typedef struct {
  int fd_id;
  char buffer[REQUEST_BUFFER_SIZE];
  long last_modified_time;
  int msg_len;
} request_buffer;

typedef struct {
  int port_num;
  char host_name[EACH_HEADER_SIZE]; // '\0' terminated
  char request_method[EACH_HEADER_SIZE]; // '\0' terminated
  char msg[REQUEST_BUFFER_SIZE]; // '\0' terminated
} request_from_client;

typedef struct {
  int body_len;
  int msg_len;
  char *msg;
  char *body;
} response_from_server;

typedef struct {
  char key[EACH_HEADER_SIZE]; // '\0' terminated
  response_from_server *value;
  long expiration_time;
} cache_ele;

request_buffer **request_buf_arr;
cache_ele **cache_arr;

int make_socket(uint16_t port);
bool is_header_end(char *str, int len);
int parse_port_number(char *str);
void client_get_request(int filedes, fd_set *active_fd_set);
request_from_client* request_parse(request_buffer *req_buf);
bool contains_chars(char *ptr, char *header, int len);
void request_buf_init();
void request_buf_new(int filedes);
void request_buf_reset(request_buffer *req_buf);
void client_finish(int filedes, fd_set *active_fd_set);
response_from_server* cache_get(request_from_client *req);
void cache_init();
cache_ele* cache_new();
void cache_reset(cache_ele *cache);
response_from_server* response_new();
void response_reset(response_from_server *response);
cache_ele* cache_find(char *key);
cache_ele* cache_next_free();
cache_ele* cache_new_or_update(cache_ele* node, request_from_client *req);
void server_get(response_from_server* response, request_from_client *req);
void response_parse(cache_ele *node);

// TO be implemented
void cache_refresh(); // active cache reset
void destory();
void server_get_head(); // TODO: get the head and parse the content-length to fasten the process


int main(int argc, char **argv) {
  int main_sock;
  fd_set active_fd_set, read_fd_set;
  struct sockaddr_in clientname;
  int proxy_port_no; // port to listen on
  
  /* 
   * check command line arguments
   */
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  proxy_port_no = atoi(argv[1]);
  
  /* Create the socket and set it up to accept connections. */
  main_sock = make_socket(proxy_port_no);
  if (listen(main_sock, 1) < 0) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  printf("Create the main socket with ID %d\n", main_sock);
  
  /* Initialize the set of active sockets. */
  FD_ZERO(&active_fd_set);
  printf("Initial FD_SETSIZE is %d\n", FD_SETSIZE);
  FD_SET(main_sock, &active_fd_set);
  printf("After main socket, FD_SETSIZE is %d\n", FD_SETSIZE);
  
  /* Init buffers */
  request_buf_init();
  cache_init();
  
  /* Initialize the Timeout */
  struct timeval interval; // refresh for each interval
  interval.tv_sec = 1;
  interval.tv_usec = 0;
  
  /* run the proxy */
  while (1) {
    /* Block until input arrives on one or more active sockets, or timeout
       expries. */
    read_fd_set = active_fd_set;
    if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, &interval) < 0) {
      perror("select");
      exit(EXIT_FAILURE);
    }

    /* Service all the sockets with input pending or close any socket w partial
       data >= 1min */
    for (int i = 0; i < FD_SETSIZE; ++i) {
      // respond to the socket
      if (FD_ISSET(i, &read_fd_set)) {
        // main socket is ready which means new client is connecting
        if (i == main_sock) {
          printf("Main socket %d is being processed\n", i);
          /* Connection request on original/main socket. */
          int size = sizeof(clientname);
          int new = accept(main_sock, (struct sockaddr *) &clientname, &size);
          if (new < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
          }
          fprintf(stderr, "\nServer: connect from host %s, port %hd.\n",
                  inet_ntoa(clientname.sin_addr), ntohs(clientname.sin_port));
          FD_SET(new, &active_fd_set);
        } else {
          //printf("\nSocket %d is being processed, current time %ld\n", i, time(NULL));
          
          /* Data arriving on an already-connected socket. */
          client_get_request(i, &active_fd_set);
          
          //printf("modified_time: %ld\n", msg_buffer_ptr[i].modified_time);
        }
      } else if (request_buf_arr[i] != NULL && request_buf_arr[i]->fd_id != -1) {
        // CLose any client which is idle for >= 1 min
        if (time(NULL) - request_buf_arr[i]->last_modified_time >= REQUEST_BUFFER_TIMEOUT_SECONDS) {
          request_buf_reset(request_buf_arr[i]);
          printf("Reset the buffer with client id %d due to timeout\n", i);
        }
      }
    }
  }
}

int make_socket(uint16_t port) {
  int sock;
  struct sockaddr_in name;

  /* Create the socket. */
  sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  /* Give the socket a name. */
  name.sin_family = AF_INET;
  name.sin_port = htons(port);
  name.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr *) &name, sizeof(name)) < 0) {
    perror("bind");
    exit(EXIT_FAILURE);
  }

  return sock;
}

bool is_header_end(char *str, int len) {
  if (str[len - 1] == '\n' && str[len - 2] == '\r' && str[len - 3] == '\n'
      && str[len - 4] == '\r') {
    return true;
  }
  return false;
}

int parse_port_number(char *str) {
  // parse port num if available
  if (str[0] == ':' && str[1] >= '0' && str[1] <= '9') {
    printf("Parsing port number\n");
    
    char port_chars[10];
    int i = 0;
    
    while (str[i + 1] >= '0' && str[i + 1] <= '9') {
      port_chars[i] = str[i + 1];
      i++;
    }
    port_chars[i] = '\0';
    
    printf("Port number is:[%s]\n", port_chars);
    
    return atoi(port_chars);
  }
  
  return 0;
}

void client_get_request(int filedes, fd_set *active_fd_set) {
  int nbytes = 0;
  request_buffer *tmp_buffer;
  
  // read from client to request_buf_arr[filedes]
  // Initialize the mem if it is NULL
  if (request_buf_arr[filedes] == NULL) {
    request_buf_new(filedes);
  }
  tmp_buffer = request_buf_arr[filedes];
  
  printf("Begin to read from %d\n", filedes);
  char *end = tmp_buffer->buffer + tmp_buffer->msg_len;
  int rest_size = REQUEST_BUFFER_SIZE - tmp_buffer->msg_len;
  tmp_buffer->last_modified_time = time(NULL);
  nbytes = read(filedes, end, rest_size);
  printf("Server: got message %d bytes of data.\n", nbytes);
  
  /* for test
  putchar('[');
  putchar('\n');
  for (int i = 0; i < nbytes; i++) {
    printf("%d ", tmp_buffer->buffer[i]);
  }
  printf("%s", tmp_buffer->buffer);
  putchar(']');
  putchar('\n');
  */
  
  if (nbytes < 0) {
    // Read error
    perror("read");
    exit(EXIT_FAILURE);
  } else if (nbytes == 0) {
    // EOF
    printf("The client closed connection!\n");
    
    client_finish(filedes, active_fd_set);
  } else if (nbytes < REQUEST_BUFFER_SIZE){
    tmp_buffer->msg_len += nbytes;
    
    // if it is a complete msg; complete msg: two consecutive '\n'
    if (tmp_buffer->msg_len > 4 && is_header_end(tmp_buffer->buffer, tmp_buffer->msg_len)) {
      // parse it
      request_from_client *request = request_parse(tmp_buffer);
      if (request == NULL) {
        perror("read: invalid request!\n");
        client_finish(filedes, active_fd_set);
        return;
      }
      
      //get the response from cache
      response_from_server *response = cache_get(request);
      
      // write response to client
      nbytes = write(filedes, response->msg, response->msg_len);
      printf("Write %d bytes data to client\n", nbytes);
      if (nbytes != response->msg_len) {
        perror("ERROR writing to client socket");
        exit(EXIT_FAILURE);
      }
      
      client_finish(filedes, active_fd_set);
    }
  } else {
    perror("read: buffer size is not large enough!");
    exit(EXIT_FAILURE);
  }
}

request_from_client* request_parse(request_buffer *req_buf) {
  printf("Begin to parse the request from client\n");
  
  // if it is a valid request, return a new request_from_client
  request_from_client *request = (request_from_client*) malloc(sizeof(request_from_client));
  request->host_name[0] = '\0';
  request->request_method[0] = '\0';
  request->port_num = 80;
  memcpy(request->msg, req_buf->buffer, req_buf->msg_len);
  request->msg[req_buf->msg_len] = '\0';
  
  int get_len = 4;
  int host_len = 6;
  char get_header[4] = "GET ";
  char host_header[6] = "Host: ";
  
  for (int i = 0; i < req_buf->msg_len; i++) {
    char *line = req_buf->buffer + i;
    if (contains_chars(line, get_header, get_len)) {
      printf("Parsing GET method\n");
      
      int j = 0;
      while (req_buf->buffer[i] != '\r') {
        int port_num_parsed = parse_port_number(line + j);
        if (port_num_parsed > 0){
          request->port_num = port_num_parsed;
        }
        
        request->request_method[j++] = req_buf->buffer[i++];
      }
      i++; // now is '\n'
      request->request_method[j] = '\0';
      printf("Get method is:[%s]\n", request->request_method);
    } else if (contains_chars(line, host_header, host_len)) {
      printf("Parsing host name\n");
      
      i += host_len;
      int j = 0;
      while (req_buf->buffer[i] != '\r') {
        request->host_name[j++] = req_buf->buffer[i++];
      }
      request->host_name[j] = '\0';
      printf("Host is:[%s]\n", request->host_name);
      i++; // now is '\n'
    } else {
      // neglect this line
      while (req_buf->buffer[i] != '\n') {
        i++;
      }
    }
  }
  
  // otherwise, return NULL
  if (strlen(request->host_name) == 0 || strlen(request->request_method) == 0) {
    free(request);
    return NULL;
  } else {
    return request;
  }
}

bool contains_chars(char *ptr, char *header, int len) {
  for (int i = 0; i < len; i++) {
    if (ptr[i] != header[i]) {
      return false;
    }
  }
  
  return true;
}

void request_buf_init() {
  request_buf_arr = (request_buffer **) calloc(FD_SETSIZE, sizeof(request_buffer*));
}

void request_buf_new(int filedes) {
  request_buf_arr[filedes] = (request_buffer*) malloc(sizeof(request_buffer));
  
  request_buf_arr[filedes]->fd_id = filedes;
  bzero(request_buf_arr[filedes]->buffer, REQUEST_BUFFER_SIZE);
  //request_buf_arr[filedes]->last_modified_time = time(NULL);
  request_buf_arr[filedes]->msg_len = 0;
}

void request_buf_reset(request_buffer *req_buf) {
  req_buf->fd_id = -1;
  bzero(req_buf->buffer, REQUEST_BUFFER_SIZE);
  req_buf->last_modified_time = 0;
  req_buf->msg_len = 0;
}

void client_finish(int filedes, fd_set *active_fd_set) {
  // reset the buffer
  request_buf_reset(request_buf_arr[filedes]);
  
  // close the connection and clear socket
  close(filedes);
  FD_CLR(filedes, active_fd_set);
}

response_from_server* cache_get(request_from_client *req) {
  // if NULL or expried, get response from server, save it and parse it  
  cache_ele* res_cache = cache_find(req->request_method);
  if (res_cache == NULL || res_cache->expiration_time <= time(NULL)) {
    printf("No result in the cache or expried, begin to get from server\n");
    res_cache = cache_new_or_update(res_cache, req);
  }
  
  // return the response
  return res_cache->value;
}

void cache_init() {
  cache_arr = (cache_ele **) malloc(CACHE_CAP * sizeof(cache_ele*));
  for (int i = 0; i < CACHE_CAP; i++) {
    cache_arr[i] = cache_new();
  }
}

cache_ele* cache_new() {
  cache_ele *res = (cache_ele *) malloc(sizeof(cache_ele));
  res->value = response_new();
  cache_reset(res);
  return res;
}

void cache_reset(cache_ele *cache) {
  cache->key[0] = '\0';
  cache->expiration_time = 0;
  response_reset(cache->value);
}

response_from_server* response_new() {
  response_from_server *res = (response_from_server *) malloc(sizeof(response_from_server));
  res->msg = (char *) malloc(RESPONSE_MSG_SIZE * sizeof(char));
  return res;
}

void response_reset(response_from_server *response) {
  bzero(response->msg, RESPONSE_MSG_SIZE);
  response->body = NULL;
  response->body_len = 0;
  response->msg_len = 0;
}

cache_ele* cache_find(char *key) {
  printf("Trying to find the key [%s] in the cache\n", key);
  for (int i = 0; i < CACHE_CAP; i++) {
    if (strcmp(key, cache_arr[i]->key) == 0) {
      printf("Found the key in the cache\n");
      return cache_arr[i];
    }
  }
  return NULL;
}

cache_ele* cache_next_free() {
  int res = CACHE_CAP; // earliest expir index
  for (int i = 0; i < CACHE_CAP; i++) {
    if (strlen(cache_arr[i]->key) == 0) {
      return cache_arr[i];
    } else if (res == CACHE_CAP
               || cache_arr[i]->expiration_time < cache_arr[res]->expiration_time) {
      res = i;
    }
  }
  
  cache_reset(cache_arr[res]);
  return cache_arr[res];
}

cache_ele* cache_new_or_update(cache_ele* node, request_from_client *req) {
  // create a new cache node if NULL
  if (node == NULL) {
    node = cache_next_free();
    strcpy(node->key, req->request_method);
    printf("New cache with key [%s]\n", req->request_method);
  }
  
  // update value and expiration_time
  server_get(node->value, req);
  response_parse(node);
  
  // return the node
  return node;
}

void server_get(response_from_server* response, request_from_client *req) {
  struct hostent *server;
  int sock_fd;
  struct sockaddr_in server_addr;
  int nbytes;
  
  // socket: create the socket
  sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("ERROR opening socket");
    exit(EXIT_FAILURE);
  }
  
  // gethostbyname: get the server's DNS entry
  server = gethostbyname(req->host_name);
  if (server == NULL) {
    fprintf(stderr,"ERROR, no such host as %s\n", req->host_name);
    exit(0);
  }
  
  // build the server's Internet address
  bzero((char *) &server_addr, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  bcopy((char *)server->h_addr, (char *)&server_addr.sin_addr.s_addr, server->h_length);
  server_addr.sin_port = htons(req->port_num);
  
  // connect: create a connection with the server
  if (connect(sock_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
    perror("ERROR connecting");
    exit(EXIT_FAILURE);      
  }
  
  // send the request to the server
  nbytes = write(sock_fd, req->msg, strlen(req->msg));
  //printf("Write msg:[\n%s\n] to the server\n", req->msg);
  if (nbytes < 0 || strlen(req->msg) != nbytes) {
    perror("ERROR writing to socket");
    exit(EXIT_FAILURE);
  }
  
  // read the reply
  printf("Begin to read from the server\n");
  nbytes = 0;
  char *tmp_buf = response->msg;
  int tmp_n = read(sock_fd, tmp_buf, RESPONSE_MSG_SIZE);
  while (tmp_n > 0) {
    printf("Read %d bytes from the server\n", tmp_n);
    tmp_buf += tmp_n;
    nbytes += tmp_n;
    tmp_n = read(sock_fd, tmp_buf, RESPONSE_MSG_SIZE - nbytes);
  }
  if (tmp_n < 0) {
    perror("ERROR reading from socket");
    exit(EXIT_FAILURE);
  }
  
  printf("Finished reading from the server with %d bytes\n", nbytes);
  response->msg_len = nbytes;
  
  close(sock_fd);
}

void response_parse(cache_ele *node) {
  int len_cache_ctrl = 15;
  int len_content = 16;
  int len_max_age = 8;
  char cache_ctrl[15] = "Cache-Control: ";
  char content_len[16] = "Content-Length: ";
  char max_age[8] = "max-age=";
  char *res_buf = node->value->msg;
  long cur_time = time(NULL);
  node->expiration_time = cur_time + DEFAULT_CACHE_AGE;
  
  /* Test
  for (int i = 0; i < node->value->msg_len; i++) {
    //printf("%d ", node->value->msg[i]);
    putchar(node->value->msg[i]);
  }
  printf("\n");
  */
  
  for (int i = 0; i < node->value->msg_len; i++) {
    char *line = res_buf + i;
    
    // the separting line between header and body '\r\n'
    if (res_buf[i] == '\r' && node->value->body == NULL) {
      printf("Get %ld bytes header from the server\n", i);
      
      i++; // '\n'
      node->value->body = res_buf + i + 1;
      
      break; // only to parse the info in the header
    // get content-length
    } else if (contains_chars(line, content_len, len_content)) {
      for (int k = 0; line[k] != '\n'; k++) {
        putchar(line[k]);
      }
      putchar('\n');
      
      char tmp_len[100];
      int j = 0;
      i += len_content;
      while (res_buf[i] != '\r') {
        tmp_len[j++] = res_buf[i++];
      }
      tmp_len[j] = '\0';
      i++; // '\n'
      
      node->value->body_len = atol(tmp_len);
      printf("Expect %ld bytes body from the server\n", node->value->body_len);
    // get max-age in Cache-Control
    } else if (contains_chars(line, cache_ctrl, len_cache_ctrl)) {
      for (int k = 0; line[k] != '\n'; k++) {
        putchar(line[k]);
      }
      putchar('\n');
      
      i += len_cache_ctrl;
      while (res_buf[i] != '\r') {
        if (contains_chars(res_buf + i, max_age, len_max_age)) {
          i += len_max_age;
          char tmp_age[100];
          int j = 0;
          while (res_buf[i] <= '9' && res_buf[i] >= '0') {
            tmp_age[j++] = res_buf[i++];
          }
          tmp_age[j] = '\0';
          node->expiration_time = cur_time + atol(tmp_age);
          
          printf("Expiration at %ld\n", node->expiration_time);
        } else {
          i++;
        }
      }
      i++; // '\n'
    // neglect this line  
    } else {
      while (res_buf[i] != '\n') {
        i++;
      }
    }
  }
}
