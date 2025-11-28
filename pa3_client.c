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

    // 소켓 생성
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket creation failed");
        return -1;
    }

    // 호스트 이름 해석 (IP 문자열 또는 도메인)
    if ((host_entry = gethostbyname(hostname)) == NULL) {
        fprintf(stderr, "invalid hostname %s\n", hostname);
        close(sockfd);
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, host_entry->h_addr_list[0], host_entry->h_length);

    // 서버 연결
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
    // 1. Action (Type) - int32_t로 명시적 변환하여 전송
    int32_t action_val = (int32_t)request->action;
    if (write(sockfd, &action_val, sizeof(int32_t)) <= 0) return;

    // 2. Lengths
    if (write(sockfd, &request->username_length, sizeof(uint64_t)) <= 0) return;
    if (write(sockfd, &request->data_size, sizeof(uint64_t)) <= 0) return;

    // 3. Values (Username)
    if (request->username_length > 0 && request->username != NULL) {
        size_t sent = 0;
        while (sent < request->username_length) {
            ssize_t n = write(sockfd, request->username + sent, request->username_length - sent);
            if (n < 0) {
                if (errno == EINTR) continue; // 시그널 인터럽트 발생 시 재시도
                return;
            }
            if (n == 0) return; // 연결 끊김
            sent += n;
        }
    }

    // 3. Values (Data)
    if (request->data_size > 0 && request->data != NULL) {
        size_t sent = 0;
        while (sent < request->data_size) {
            ssize_t n = write(sockfd, request->data + sent, request->data_size - sent);
            if (n < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (n == 0) return;
            sent += n;
        }
    }
}

// -------------------------------------
// receive_response
// -------------------------------------
void receive_response(int32_t sockfd, Response* response) {
    // 초기화: 통신 실패 시에도 쓰레기 값을 갖지 않도록 함
    memset(response, 0, sizeof(Response));

    // 1. Length (Data Size)
    if (recv(sockfd, &response->data_size, sizeof(uint64_t), MSG_WAITALL) <= 0) return;

    // 2. Type (Code)
    if (recv(sockfd, &response->code, sizeof(int32_t), MSG_WAITALL) <= 0) return;

    // 3. Value (Data)
    if (response->data_size > 0) {
        response->data = malloc(response->data_size);
        if (!response->data) {
            perror("malloc failed");
            response->data_size = 0;
            return;
        }

        ssize_t received = recv(sockfd, response->data, response->data_size, MSG_WAITALL);
        if (received <= 0) {
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
    // 로그인된 사용자가 있을 때만 로그아웃 요청 전송
    if (active_user != NULL) {
        Request req;
        // Request 초기화 (garbage value 방지)
        memset(&req, 0, sizeof(Request));

        req.action = ACTION_LOGOUT;
        // 주의: helper.c와 일관성을 위해 strlen 사용 (널 문자 제외 길이)
        req.username_length = strlen(active_user); 
        req.username = strdup(active_user);
        req.data_size = 0;
        req.data = NULL;

        send_request(sockfd, &req);

        Response res;
        receive_response(sockfd, &res);

        // handle_response 호출하여 결과 출력 및 active_user 정리
        // handle_response 내부에서 active_user를 free하고 NULL로 설정함
        handle_response(ACTION_LOGOUT, &req, &res, &active_user);

        // 자원 해제
        if (req.username) free(req.username);
        if (res.data) free(res.data);
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

    // 소켓 연결
    int32_t sockfd = get_socket(argv[1], strtoull(argv[2], NULL, 10));
    if (sockfd < 0) exit(EXIT_FAILURE);

    // 파일 모드
    if (argc > 3) {
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
            // 개행 문자 제거
            if (nread > 0 && line[nread - 1] == '\n') {
                line[nread - 1] = '\0';
            }

            if (!line_is_empty(line)) {
                if (!evaluate(line, sockfd, &active_user)) {
                    // "exit" 등의 액션으로 루프 종료 시
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
    } 
    // 인터랙티브 모드 (REPL)
    else {
        while (true) {
            char* input = readline(""); // 프롬프트 없이 입력 대기
            
            // EOF(Ctrl+D) 또는 SIGINT 수신 시 종료
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

    terminate(sockfd, active_user);
    close(sockfd);
    return 0;
}
