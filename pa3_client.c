#include <arpa/inet.h>
#include <ctype.h>
#include <editline/readline.h>
#include <errno.h>
#include <netinet/in.h>
#include <pa3_error.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "handle_response.h"
#include "helper.h"

const char* active_user = NULL;
bool sigint_received = false;

// -------------------------------------
// get_socket
// -------------------------------------
int32_t get_socket(char* hostname, uint64_t port) {
  int sockfd;
  struct hostent* host_entry;
  struct sockaddr_in serv_addr;

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket creation failed");
    return -1;
  }

  if ((host_entry = gethostbyname(hostname)) == NULL) {
    fprintf(stderr, "invalid hostname %s\n", hostname);
    close(sockfd);
    return -1;
  }

  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  memcpy(&serv_addr.sin_addr.s_addr, host_entry->h_addr_list[0], host_entry->h_length);

  if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    perror("connection failed");
    close(sockfd);
    return -1;
  }

  return sockfd;
}

// -------------------------------------
// send_request
// -------------------------------------
void send_request(int32_t sockfd, Request* request) {
  // action
  write(sockfd, &request->action, sizeof(request->action));

  // username length
  write(sockfd, &request->username_length, sizeof(request->username_length));

  // data size
  write(sockfd, &request->data_size, sizeof(request->data_size));

  // username
  if (request->username_length > 0 && request->username != NULL) {
    size_t sent = 0;
    while (sent < request->username_length) {
      ssize_t n = write(sockfd,
                        request->username + sent,
                        request->username_length - sent);
      if (n <= 0) return;
      sent += n;
    }
  }

  // data
  if (request->data_size > 0 && request->data != NULL) {
    size_t sent = 0;
    while (sent < request->data_size) {
      ssize_t n = write(sockfd,
                        request->data + sent,
                        request->data_size - sent);
      if (n <= 0) return;
      sent += n;
    }
  }
}

// -------------------------------------
// receive_response
// -------------------------------------
void receive_response(int32_t sockfd, Response* response) {
  response->data = NULL;
  response->data_size = 0;

  if (recv(sockfd, &response->data_size, sizeof(uint64_t), MSG_WAITALL) <= 0) return;
  if (recv(sockfd, &response->code, sizeof(int32_t), MSG_WAITALL) <= 0) return;

  if (response->data_size > 0) {
    response->data = malloc(response->data_size);
    if (!response->data) {
      response->data_size = 0;
      return;
    }

    if (recv(sockfd, response->data, response->data_size, MSG_WAITALL) <= 0) {
      free(response->data);
      response->data = NULL;
      response->data_size = 0;
    }
  }
}

// -------------------------------------
// terminate
// -------------------------------------
void terminate(int32_t sockfd, const char* active_user) {
  if (active_user != NULL) {
    Request req;
    default_request(&req);

    req.action = ACTION_LOGOUT;
    req.username_length = strlen(active_user) + 1;
    req.username = strdup(active_user);
    req.data_size = 0;

    send_request(sockfd, &req);

    Response res;
    receive_response(sockfd, &res);

    const char* tmp = active_user;
    handle_response(ACTION_LOGOUT, &req, &res, &tmp);

    free_request(&req);
    free_response(&res);
  }
}

// -------------------------------------
// main
// -------------------------------------
int main(int argc, char* argv[]) {
  setup_sigint_handler();

  if (argc < 3) {
    fprintf(stderr, "usage: %s <IP address> <port>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  int32_t sockfd = get_socket(argv[1], strtoull(argv[2], NULL, 10));
  if (sockfd < 0) exit(EXIT_FAILURE);

  if (argc > 3) {
    const char* filename = argv[3];
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
      fprintf(stderr, "%s: %s: %s\n", argv[0], filename, strerror(errno));
      exit(1);
    }
   
    char* line = NULL;
    size_t len = 0;

    while (getline(&line, &len, file) != -1) {
      size_t L = strlen(line);
      if (L > 0 && line[L - 1] == '\n') line[L - 1] = '\0';

      if (!line_is_empty(line)) {
        if (!evaluate(line, sockfd, &active_user)) {
          free(line);
          fclose(file);
          terminate(sockfd, active_user);
          close(sockfd);
          return 0;
        }
      }

      free(line);
      line = NULL;
      len = 0;
    }

    fclose(file);
  } else {
    while (true) {
      char* input = readline("");
      if (input == NULL || sigint_received) break;

      add_history(input);

      if (!evaluate(input, sockfd, &active_user)) {
        free(input);
        break;
      }

      free(input);
    }
  }

  terminate(sockfd, active_user);
  close(sockfd);
  return 0;
}
