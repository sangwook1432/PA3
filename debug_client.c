#include <arpa/inet.h>
#include <ctype.h>
#include <editline/readline.h>
#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
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
    // 1. Action
    int32_t action_val = (int32_t)request->action;
    if (write(sockfd, &action_val, sizeof(int32_t)) <= 0) return;

    // 2. Lengths
    if (write(sockfd, &request->username_length, sizeof(uint64_t)) <= 0) return;
    if (write(sockfd, &request->data_size, sizeof(uint64_t)) <= 0) return;

    // 3. Username
    if (request->username_length > 0 && request->username != NULL) {
        size_t sent = 0;
        while (sent < request->username_length) {
            ssize_t n = write(sockfd, request->username + sent, request->username_length - sent);
            if (n < 0) { if (errno == EINTR) continue; return; }
            if (n == 0) return;
            sent += n;
        }
    }

    // 4. Data
    if (request->data_size > 0 && request->data != NULL) {
        size_t sent = 0;
        while (sent < request->data_size) {
            ssize_t n = write(sockfd, request->data + sent, request->data_size - sent);
            if (n < 0) { if (errno == EINTR) continue; return; }
            if (n == 0) return;
            sent += n;
        }
    }
}

// -------------------------------------
// receive_response
// -------------------------------------
void receive_response(int32_t sockfd, Response* response) {
    // 구조체 초기화 (쓰레기 값 방지)
    memset(response, 0, sizeof(Response));

    uint64_t size_buf = 0;
    // recv가 0이나 -1을 반환하면 연결 종료/에러
    if (recv(sockfd, &size_buf, sizeof(uint64_t), MSG_WAITALL) <= 0) return;
    response->data_size = size_buf;

    int32_t code_buf = 0;
    if (recv(sockfd, &code_buf, sizeof(int32_t), MSG_WAITALL) <= 0) return;
    response->code = code_buf;

    if (response->data_size > 0) {
        // 비정상적으로 큰 데이터 사이즈 방어 (10MB 제한)
        if (response->data_size > 10 * 1024 * 1024) {
             printf("[DEBUG] Too large data size: %lu\n", response->data_size);
             return; 
        }

        response->data = malloc(response->data_size + 1);
        if (!response->data) {
            perror("malloc failed");
            response->data_size = 0;
            return;
        }
        
        // 메모리 0으로 초기화
        memset(response->data, 0, response->data_size + 1);

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
        printf("[DEBUG] Terminate started for user: %s\n", active_user);
        
        Request req;
        memset(&req, 0, sizeof(Request));

        req.action = ACTION_LOGOUT;
        // active_user 문자열을 복사해서 사용 (strdup 필수)
        req.username_length = strlen(active_user); 
        req.username = strdup(active_user);
        req.data_size = 0;
        req.data = NULL;

        send_request(sockfd, &req);
        printf("[DEBUG] Logout sent\n");

        Response res;
        memset(&res, 0, sizeof(Response)); // 중요: 초기화

        receive_response(sockfd, &res);
        printf("[DEBUG] Logout response received. Code: %d\n", res.code);

        // handle_response 호출
        // 주의: active_user 포인터를 넘기면 내부에서 free를 시도할 수 있음.
        // 여기서는 복사본(tmp) 주소를 넘겨서 안전하게 처리
        const char* tmp_user = active_user; 
        handle_response(ACTION_LOGOUT, &req, &res, &tmp_user);
        printf("[DEBUG] handle_response finished\n");

        // Request의 username 해제 (우리가 malloc했으므로 우리가 free)
        if (req.username) {
            free(req.username);
            req.username = NULL;
        }
        
        // Response의 data 해제
        // handle_response가 return 문으로 일찍 종료되면 data가 free되지 않을 수 있음.
        // 안전하게 여기서 체크 후 해제.
        if (res.data != NULL) {
            free(res.data);
            res.data = NULL;
        }
        printf("[DEBUG] Terminate finished cleanup\n");
    }
}

// -------------------------------------
// main
// -------------------------------------
int main(int argc, char* argv[]) {
    // 1. SIGPIPE 시그널 무시 (서버 끊김 시 Crash 방지)
    signal(SIGPIPE, SIG_IGN);
    setup_sigint_handler();

    if (argc < 3) {
        fprintf(stderr, "usage: %s <IP address> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int32_t sockfd = get_socket(argv[1], strtoull(argv[2], NULL, 10));
    if (sockfd < 0) exit(EXIT_FAILURE);

    if (argc > 3) {
        // --- FILE MODE ---
        const char* filename = argv[3];
        FILE* file = fopen(filename, "r");
        if (file == NULL) {
            fprintf(stderr, "%s: %s: %s\n", argv[0], filename, strerror(errno));
            close(sockfd);
            exit(1);
        }
       
        char* line = NULL;
        size_t len = 0;
        ssize_t nread;

        while ((nread = getline(&line, &len, file)) != -1) {
            if (nread > 0 && line[nread - 1] == '\n') line[nread - 1] = '\0';

            if (!line_is_empty(line)) {
                if (!evaluate(line, sockfd, &active_user)) {
                    free(line);
                    fclose(file);
                    terminate(sockfd, active_user);
                    close(sockfd);
                    return 0;
                }
            }
        }
        free(line);
        fclose(file);
    } else {
        // --- INTERACTIVE MODE ---
        while (true) {
            char* input = readline(""); 
            
            if (input == NULL || sigint_received) {
                if (input) free(input);
                break;
            }

            add_history(input);

            if (!evaluate(input, sockfd, &active_user)) {
                free(input);
                break;
            }

            free(input);
        }
    }

    printf("[DEBUG] Main loop ended. Calling terminate...\n");
    terminate(sockfd, active_user);
    
    printf("[DEBUG] Closing socket...\n");
    close(sockfd);
    
    printf("[DEBUG] Exiting success\n");
    return 0;
}
