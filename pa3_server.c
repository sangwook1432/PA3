#include <arpa/inet.h>
#include <errno.h>
#include <helper.h>
#include <netinet/in.h>
#include <pa3_error.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "helper.h"

bool sigint_received = false;

// Helper function: can write to fd safely even when sigint is received
ssize_t sigint_safe_write(int32_t fd, void* buf, size_t count) {
  ssize_t n_written;
  do {
    n_written = write(fd, buf, count);
  } while (n_written < 0 && errno == EINTR);
  return n_written;
}

// Helper function: can read from fd safely even when sigint is received
ssize_t sigint_safe_read(int32_t fd, void* buf, size_t count) {
  ssize_t n_read;
  do {
    n_read = read(fd, buf, count);
  } while (n_read < 0 && errno == EINTR);
  return n_read;
}

// Use pthread_mutex_lock when accessing 'PollSet'
void add_to_pollset(PollSet* poll_set,
                    int32_t notification_fd,
                    int32_t connfd) {
  // ???
  pthread_mutex_lock(&poll_set->mutex);
  if(poll_set->size < CLIENTS_PER_THREAD){
    poll_set->set[poll_set->size].fd = connfd;
    poll_set->set[poll_set->size].events = POLLIN;
    poll_set->size++;
  }
  pthrea_mutex_unlock(&poll_set->mutex);
  notify_pollset(notification_fd);
  // ???
}

// This function is called within thread_func.
// Assuming you have already obtained the lock in thread_func, you do not need to lock the mutex here.
void remove_from_pollset(ThreadData* data, size_t* i_ptr) {
  // ???
  size_t i = *i_ptr;
  close(data->poll_set->set[i].fd);
  data->poll_set->set[i] = data->poll_set->set[data->poll_set->size - 1];
  data->poll_set->size--;
  (*i_ptr)--;
}

// You have to poll the poll_set. When there is a file that is ready to be read,
// you have to lock the poll set to prevent cases where the poll set gets
// updated. You have to remember to unlock the poll set after you are done
// reading from it, including cases where you have to exit due to errors, or
// else your program might not be able to terminate without calling (p)kill.
void* thread_func(void* arg) {
  ThreadData* data = (ThreadData*)arg;
  while (!sigint_received) {
    // ???
    int ret = poll(data->poll_set->set,data->poll_set->size, -1);
    if(ret < 0){
      if(errno == EINTR) continue;
      break;
    }
    pthread_mutex_lock(&data->poll_set->mutex);
    if(data->poll_set->set[0].revents & POLLIN){
      char buf[16];
      sigint_safe_read(data->poll_set->set[0].fd,buf,sizeof(buf));
    }
    for(size_t i = 1;i < data->poll_set->size;i++){
      if(data->poll_set->set[i].revents & (POLLIN | POLLERR | POLLHUP)){
        int connfd = data->poll_set->set[i].fd;
        Request request = {0};
        if(sigint_safe_read(connfd,&request.action,sizeof(request.action)) <= 0){
          remove_from_pollset(data,&i);
          continue;
        }
        sigint_safe_read(connfd,&request.username_length,sizeof(request.username_length));
        sigint_safe_read(connfd,&request.data_size,sizeof(request.data_size));
        if(request.username_length > 0){
          request.username = malloc(request.username_length);
          sigint_safe_read(connfd,request.username,request.username_length);
        }
        if(request.data_size > 0){
          request.data = malloc(request.data_size);
          sigint_safe_read(connfd,request.data,request.data_size);
        }

        Response response = {0};
        response.code = handle_request(&request,&response,data->users,data->seats);
        sigint_safe_write(connfd,&response.data_size,sizeof(response.data_size));
        sigint_safe_write(connfd,&response.code,sizeof(response.code));
        if(response.data_size > 0 && response.data){
          sigint_safe_write(connfd,response.data,response.data_size);
        }
        if(request.username) free(request.username);
        if(request.data) free(request.data);
        if(response.data) free(response.data);
      }
    }
    pthread_mutex_unlock(&data->poll_set->mutex);
  }
  pthread_exit(NULL);
}

int main(int argc, char* argv[]) {
  setup_sigint_handler();

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    return 1;
  }

  int listenfd;
  struct sockaddr_in saddr, caddr;

  Users users;
  setup_users(&users);

  Seat* seats = default_seats();
  int32_t n_cores = get_num_cores();

  pthread_t* tid_arr = malloc(sizeof(pthread_t) * n_cores);
  ThreadData* data_arr = malloc(sizeof(ThreadData) * n_cores);
  int32_t (*pipe_fds)[2] = malloc(sizeof(int32_t[2]) * n_cores);

  for (int i = 0; i < n_cores; i++) {
    if (pipe(pipe_fds[i]) < 0) {
      perror("pipe");
      exit(EXIT_FAILURE);
    }

    data_arr[i].thread_index = i;
    data_arr[i].pipe_out_fd = pipe_fds[i][0];
    data_arr[i].poll_set = create_poll_set(pipe_fds[i][0]);
    data_arr[i].users = &users;
    data_arr[i].seats = seats;
    pthread_create(&tid_arr[i], nullptr, thread_func, &data_arr[i]);
  }

  listenfd = socket(AF_INET, SOCK_STREAM, 0);

  memset(&saddr, 0, sizeof(saddr));
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = htonl(INADDR_ANY);
  saddr.sin_port = htons(strtoull(argv[1], nullptr, 10));

  bind(listenfd, (struct sockaddr*)&saddr, sizeof(saddr));
  listen(listenfd, 10);

  struct pollfd main_thread_poll_set[2];
  memset(main_thread_poll_set, 0, sizeof(main_thread_poll_set));
  main_thread_poll_set[0].fd = STDIN_FILENO;
  main_thread_poll_set[0].events = POLLIN;
  main_thread_poll_set[1].fd = listenfd;
  main_thread_poll_set[1].events = POLLIN;

  while (!sigint_received) {
    if (poll(main_thread_poll_set, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("poll");
      exit(EXIT_FAILURE);
    }

    if (main_thread_poll_set[0].revents & POLLIN) {
      if (check_stdin_for_termination() == true) {
        kill(getpid(), SIGINT);
        continue;
      }
    } else if (main_thread_poll_set[1].revents & POLLIN) {
      uint32_t caddrlen = sizeof(caddr);
      int connfd = accept(listenfd, (struct sockaddr*)&caddr, &caddrlen);
      if (connfd < 0) {
        if (errno == EINTR) {
          continue;
        }
        puts("accept() failed");
        exit(EXIT_FAILURE);
      }

      printf("Accepted connection from client\n");
      ssize_t pollset_i;
      do {
        pollset_i = find_suitable_pollset(data_arr, n_cores);
      } while (pollset_i == -1);
      add_to_pollset(data_arr[pollset_i].poll_set, pipe_fds[pollset_i][1],
                     connfd);
    }
  }

  return terminate_after_cleanup(pipe_fds, tid_arr, data_arr, n_cores, listenfd,
                                 &users, seats);
}