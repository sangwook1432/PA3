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
  pthread_mutex_lock(&poll_set->mutex);
  if (poll_set->size < CLIENTS_PER_THREAD) {
    poll_set->set[poll_set->size].fd = connfd;
    poll_set->set[poll_set->size].events = POLLIN;
    poll_set->size++;
  }
  pthread_mutex_unlock(&poll_set->mutex);
  notify_pollset(notification_fd);
}

// This function is called within thread_func.
// Assuming you have already obtained the lock in thread_func, you do not need to lock the mutex here.
void remove_from_pollset(ThreadData* data, size_t* i_ptr) {
  // 현재 인덱스(*i_ptr)에 있는 fd를 닫고 제거
  size_t idx = *i_ptr;
  int fd = data->poll_set->set[idx].fd;
  close(fd);

  // 마지막 요소를 현재 위치로 이동 (순서는 중요하지 않음)
  data->poll_set->set[idx] = data->poll_set->set[data->poll_set->size - 1];
  data->poll_set->size--;

  // 인덱스를 하나 줄여서 다음 반복 때 현재 위치(새로 옮겨진 요소)를 다시 검사하게 함
  (*i_ptr)--;
}

// You have to poll the poll_set. When there is a file that is ready to be read,
// you have to lock the poll set to prevent cases where the poll set gets
// updated. You have to remember to unlock the poll set after you are done
// reading from it, including cases where you have to exit due to errors, or
// else your program might not be able to terminate without calling (p)kill.
void* thread_func(void* arg) {
  ThreadData* data = (ThreadData*)arg;
  
  // 로컬 폴링 배열 (PollSet 복사본)
  struct pollfd local_fds[CLIENTS_PER_THREAD];
  
  while (!sigint_received) {
    // 1. 공유 자원(PollSet)을 로컬로 복사
    //    poll() 함수가 블로킹되는 동안 Mutex를 잡고 있으면 add_to_pollset이 막히므로 복사해서 사용
    pthread_mutex_lock(&data->poll_set->mutex);
    size_t current_size = data->poll_set->size;
    memcpy(local_fds, data->poll_set->set, current_size * sizeof(struct pollfd));
    pthread_mutex_unlock(&data->poll_set->mutex);

    // 2. poll 실행
    if (poll(local_fds, current_size, -1) < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      break;
    }

    // 3. 이벤트 처리
    for (size_t i = 0; i < current_size; i++) {
      if (local_fds[i].revents & POLLIN) {
        int fd = local_fds[i].fd;

        // Case A: 파이프 알림 (새 클라이언트 연결 등)
        if (fd == data->pipe_out_fd) { // pipe_out_fd는 항상 set[0]에 있어야 함 (create_poll_set 참조)
          char buf[1];
          read(fd, buf, 1); // 파이프 비우기
          // 루프를 돌면서 다시 복사본을 갱신하러 감
        } 
        // Case B: 클라이언트 요청
        else {
          Request req;
          Response res;
          memset(&req, 0, sizeof(Request));
          memset(&res, 0, sizeof(Response));
          
          bool connection_closed = false;

          // --- Receive Request (TLV) ---
          // 1. Action (Type)
          int32_t action_val;
          if (sigint_safe_read(fd, &action_val, sizeof(int32_t)) <= 0) connection_closed = true;
          req.action = (Action)action_val;

          // 2. Lengths
          if (!connection_closed && sigint_safe_read(fd, &req.username_length, sizeof(uint64_t)) <= 0) connection_closed = true;
          if (!connection_closed && sigint_safe_read(fd, &req.data_size, sizeof(uint64_t)) <= 0) connection_closed = true;

          // 3. Values (Username)
          if (!connection_closed && req.username_length > 0) {
            req.username = malloc(req.username_length + 1); // 안전을 위해 +1
            if (sigint_safe_read(fd, req.username, req.username_length) <= 0) connection_closed = true;
            else req.username[req.username_length] = '\0'; // 문자열 보장 (옵션)
          }

          // 3. Values (Data)
          if (!connection_closed && req.data_size > 0) {
            req.data = malloc(req.data_size + 1);
            if (sigint_safe_read(fd, req.data, req.data_size) <= 0) connection_closed = true;
            else req.data[req.data_size] = '\0';
          }

          if (connection_closed) {
            // 연결 종료 처리
            if (req.username) free(req.username);
            if (req.data) free(req.data);
            
            pthread_mutex_lock(&data->poll_set->mutex);
            // 공유 PollSet에서 해당 FD를 찾아 제거해야 함
            // 로컬 인덱스 i와 공유 인덱스가 다를 수 있으므로 FD로 찾음
            for (size_t k = 0; k < data->poll_set->size; k++) {
                if (data->poll_set->set[k].fd == fd) {
                    remove_from_pollset(data, &k);
                    break;
                }
            }
            pthread_mutex_unlock(&data->poll_set->mutex);
            continue;
          }

          // --- Process Request ---
          handle_request(&req, &res, data->users, data->seats);

          // --- Send Response (TLV) ---
          // 1. Length
          sigint_safe_write(fd, &res.data_size, sizeof(uint64_t));
          // 2. Type (Code)
          sigint_safe_write(fd, &res.code, sizeof(int32_t));
          // 3. Value
          if (res.data_size > 0 && res.data != NULL) {
            sigint_safe_write(fd, res.data, res.data_size);
          }

          // --- Cleanup ---
          if (req.username) free(req.username);
          if (req.data) free(req.data);
          if (res.data) free(res.data);
        }
      }
    }
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
    data_arr[i].pipe_out_fd = pipe_fds[i][0]; // 읽기 전용 파이프
    data_arr[i].poll_set = create_poll_set(pipe_fds[i][0]);
    data_arr[i].users = &users;
    data_arr[i].seats = seats;
    pthread_create(&tid_arr[i], NULL, thread_func, &data_arr[i]);
  }

  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 빠른 재실행을 위해 추가 권장

  memset(&saddr, 0, sizeof(saddr));
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = htonl(INADDR_ANY);
  saddr.sin_port = htons(strtoull(argv[1], NULL, 10));

  if (bind(listenfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
      perror("bind failed");
      exit(EXIT_FAILURE);
  }
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
