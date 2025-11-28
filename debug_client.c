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
    // SIGPIPE 발생 시 프로그램 종료 방지 (OS 레벨)
    // 아래 main에서 SIG_IGN 처리를 하지만, send 로직 내에서도 방어적으로 작성

    int32_t action_val = (int32_t)request->action;
    if (write(sockfd, &action_val, sizeof(int32_t)) <= 0) return;

    if (write(sockfd, &request->username_length, sizeof(uint64_t)) <= 0) return;
    if (write(sockfd, &request->data_size, sizeof(uint64_t)) <= 0) return;

    if (request->username_length > 0 && request->username != NULL) {
        size_t sent = 0;
        while (sent < request->username_length) {
            ssize_t n = write(sockfd, request->username + sent, request->username_length - sent);
            if (n < 0) { if (errno == EINTR) continue; return; }
            if (n == 0) return;
            sent += n;
        }
    }

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
    // [DEBUG] 수신 시작
    memset(response, 0, sizeof(Response)); // 0으로 밀어버림 (안전장치)

    uint64_t len_buf = 0;
    ssize_t res = recv(sockfd, &len_buf, sizeof(uint64_t), MSG_WAITALL);
    if (res <= 0) return; 
    response->data_size = len_buf;

    int32_t code_buf = 0;
    if (recv(sockfd, &code_buf, sizeof(int32_t), MSG_WAITALL) <= 0) return;
    response->code = code_buf;

    if (response->data_size > 0) {
        // 비정상적으로 큰 사이즈 체크 (메모리 보호)
        if (response->data_size > 100 * 1024 * 1024) { 
            fprintf(stderr, "[ERROR] Response size too big: %lu\n", response->data_size);
            response->data_size = 0;
            return;
        }

        response->data = malloc(response->data_size + 1); // +1 여유 공간
        if (!response->data) {
            perror("malloc failed");
            response->data_size = 0;
            return;
        }
        
        // 메모리 초기화 (valgrind 에러 방지)
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
        printf("[DEBUG] Terminating session for user: %s\n", active_user);
        Request req;
        memset(&req, 0, sizeof(Request)); // 스택 메모리 쓰레기값 제거

        req.action = ACTION_LOGOUT;
        req.username_length = strlen(active_user); 
        req.username = strdup(active_user);
        req.data_size = 0;
        req.data = NULL;

        send_request(sockfd, &req);
        printf("[DEBUG] Logout request sent.\n");

        Response res;
        memset(&res, 0, sizeof(Response)); // 스택 메모리 초기화

        receive_response(sockfd, &res);
        printf("[DEBUG] Logout response received. Code: %d\n", res.code);

        // handle_response 내부에서 res.data를 free합니다.
        // active_user 문자열 처리 주의
        const char* tmp_user = active_user; 
        handle_response(ACTION_LOGOUT, &req, &res, &tmp_user);
        printf("[DEBUG] handle_response finished.\n");

        if (req.username) free(req.username);
        // if (res.data) free(res.data); <-- 삭제됨 (handle_response가 해제함)
    }
}

// -------------------------------------
// main
// -------------------------------------
int main(int argc, char* argv[]) {
    // 1. SIGPIPE 무시 설정 (서버가 끊겨도 클라이언트가 죽지 않게 함)
    signal(SIGPIPE, SIG_IGN);
    setup_sigint_handler();

    if (argc < 3) {
        fprintf(stderr, "usage: %s <IP address> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int32_t sockfd = get_socket(argv[1], strtoull(argv[2], NULL, 10));
    if (sockfd < 0) exit(EXIT_FAILURE);

    if (argc > 3) {
        // --- 파일 모드 ---
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
        // --- 인터랙티브 모드 ---
        while (true) {
            char* input = readline(""); 
            
            if (input == NULL || sigint_received) {
                if (input) free(input);
                break;
            }

            add_history(input);

            // evaluate 내부에서 문제가 생기는지 확인하기 위해 출력
            // printf("[DEBUG] Processing input: %s\n", input); 
            
            if (!evaluate(input, sockfd, &active_user)) {
                free(input);
                break;
            }

            free(input);
        }
    }

    printf("[DEBUG] Main loop finished. Calling terminate...\n");
    terminate(sockfd, active_user);
    
    printf("[DEBUG] Closing socket...\n");
    close(sockfd);
    
    printf("[DEBUG] Exiting main.\n");
    return 0;
}
