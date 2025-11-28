#include <helper.h>
#include <pa3_error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "helper.h"

// Users 배열 접근을 보호하기 위한 정적 뮤텍스 (Users 구조체에 락이 없으므로 추가)
static pthread_mutex_t user_mutex = PTHREAD_MUTEX_INITIALIZER;

// Helper: 문자열이 숫자인지 확인
int is_number(const char* str) {
  if (!str || *str == '\0') return 0;
  char* endptr;
  strtoull(str, &endptr, 10);
  return *endptr == '\0';
}

LoginErrorCode handle_login_request(const Request* request,
                                    Response* response,
                                    Users* users) {
  if (request->data_size == 0 || request->data == NULL) {
    return LOGIN_ERROR_NO_PASSWORD; // [cite: 214]
  }

  char* username = request->username;
  char* password = request->data; // Login action uses data field for password [cite: 171]

  pthread_mutex_lock(&user_mutex);

  ssize_t uid = find_user(users, username);

  if (uid == -1) {
    // 신규 유저 등록 [cite: 197-201]
    char hashed_password[HASHED_PASSWORD_SIZE];
    hash_password(password, hashed_password);
    size_t new_uid = add_user(users, username, hashed_password);
    users->array[new_uid].logged_in = true;
    pthread_mutex_unlock(&user_mutex);
    return LOGIN_ERROR_SUCCESS;
  } else {
    // 기존 유저 로그인 [cite: 204-206]
    User* user = &users->array[uid];

    // 이미 로그인되어 있는지 확인 [cite: 214]
    if (user->logged_in) {
      pthread_mutex_unlock(&user_mutex);
      return LOGIN_ERROR_ACTIVE_USER;
    }

    // 비밀번호 검증
    if (validate_password(password, user->hashed_password)) {
      user->logged_in = true;
      pthread_mutex_unlock(&user_mutex);
      return LOGIN_ERROR_SUCCESS;
    } else {
      pthread_mutex_unlock(&user_mutex);
      return LOGIN_ERROR_INCORRECT_PASSWORD;
    }
  }
}

BookErrorCode handle_book_request(const Request* request,
                                  Response* response,
                                  Users* users,
                                  Seat* seats) {
  // 1. 데이터 유효성 검사 (Lock 불필요)
  if (request->data_size == 0 || request->data == NULL) {
    return BOOK_ERROR_NO_DATA; 
  }

  // [최적화] 좌석 번호 파싱을 먼저 수행 (Lock 잡기 전에 수행하여 병목 최소화)
  if (!is_number(request->data)) {
      return BOOK_ERROR_SEAT_OUT_OF_RANGE;
  }
  int seat_id = atoi(request->data);
  if (seat_id < 1 || seat_id > NUM_SEATS) {
    return BOOK_ERROR_SEAT_OUT_OF_RANGE; 
  }

  // ---------------- CRITICAL SECTION START ----------------
  // 2. 유저 로그인 상태 확인 (user_mutex Lock 시작)
  pthread_mutex_lock(&user_mutex);
  
  ssize_t uid = find_user(users, request->username);
  if (uid == -1 || !users->array[uid].logged_in) {
    pthread_mutex_unlock(&user_mutex); // 실패 시 바로 Unlock
    return BOOK_ERROR_USER_NOT_LOGGED_IN; 
  }
  
  // [중요 수정] 여기서 user_mutex를 풀지 않습니다!
  // 예약이 끝날 때까지 로그아웃을 못하게 막아야 합니다.

  // 3. 좌석 예약 (seat mutex Lock 시작)
  // Lock 순서: User -> Seat (Deadlock 방지를 위해 순서 유지 필수)
  Seat* seat = &seats[seat_id - 1]; 
  pthread_mutex_lock(&seat->mutex);

  if (seat->user_who_booked != NULL) {
    pthread_mutex_unlock(&seat->mutex);
    pthread_mutex_unlock(&user_mutex); // [중요] 나갈 때 두 락을 모두 풀어야 함
    return BOOK_ERROR_SEAT_UNAVAILABLE; 
  }

  // 실제 예약 수행
  // (strdup 실패 가능성에 대한 방어 코드는 선택사항이지만 넣는 게 좋습니다)
  char* new_booking = strdup(request->username);
  if (new_booking == NULL) {
      pthread_mutex_unlock(&seat->mutex);
      pthread_mutex_unlock(&user_mutex);
      return BOOK_ERROR_SUCCESS; // 혹은 적절한 서버 에러 처리
  }
  seat->user_who_booked = new_booking;
  seat->amount_of_times_booked++;
  
  pthread_mutex_unlock(&seat->mutex);
  pthread_mutex_unlock(&user_mutex); // 모든 작업이 끝나고 User Lock 해제
  // ---------------- CRITICAL SECTION END ----------------

  return BOOK_ERROR_SUCCESS;
}

ConfirmBookingErrorCode handle_confirm_booking_request(const Request* request,
                                                       Response* response,
                                                       Users* users,
                                                       Seat* seats) {
  if (request->data_size == 0 || request->data == NULL) {
      return CONFIRM_BOOKING_ERROR_NO_DATA;
  }

  // 1. 유저 로그인 확인
  pthread_mutex_lock(&user_mutex);
  ssize_t uid = find_user(users, request->username);
  if (uid == -1 || !users->array[uid].logged_in) {
    pthread_mutex_unlock(&user_mutex);
    return CONFIRM_BOOKING_ERROR_USER_NOT_LOGGED_IN;
  }
  pthread_mutex_unlock(&user_mutex);

  // 2. 요청 타입 확인 ("available" or "booked")
  bool check_available = false;
  if (strcmp(request->data, "available") == 0) {
    check_available = true;
  } else if (strcmp(request->data, "booked") == 0) {
    check_available = false;
  } else {
    return CONFIRM_BOOKING_ERROR_INVALID_DATA;
  }

  // 3. 좌석 정보 수집
  // 응답 데이터는 size_t(pa3_seat_t) 배열이어야 함. 최대 100개.
  size_t* result_array = malloc(sizeof(size_t) * NUM_SEATS);
  int count = 0;

  for (int i = 0; i < NUM_SEATS; i++) {
    pthread_mutex_lock(&seats[i].mutex);
    bool condition_met = false;
    
    if (check_available) {
      if (seats[i].user_who_booked == NULL) condition_met = true;
    } else {
      if (seats[i].user_who_booked != NULL && 
          strcmp(seats[i].user_who_booked, request->username) == 0) {
        condition_met = true;
      }
    }

    if (condition_met) {
      result_array[count++] = seats[i].id;
    }
    pthread_mutex_unlock(&seats[i].mutex);
  }

  // 4. 응답 설정
  response->data = (uint8_t*)result_array;
  response->data_size = count * sizeof(size_t);
  
  // 만약 데이터가 없으면 free하고 NULL 처리 (프로토콜상 size 0이면 data 무시됨)
  if (count == 0) {
      free(result_array);
      response->data = NULL;
  }

  return CONFIRM_BOOKING_ERROR_SUCCESS;
}

int32_t handle_cancel_booking_request(const Request* request,
                                      Response* response,
                                      Users* users,
                                      Seat* seats) {
  if (request->data_size == 0 || request->data == NULL) {
      return CANCEL_BOOKING_ERROR_NO_DATA;
  }

  // 1. 유저 로그인 확인
  pthread_mutex_lock(&user_mutex);
  ssize_t uid = find_user(users, request->username);
  if (uid == -1 || !users->array[uid].logged_in) {
    pthread_mutex_unlock(&user_mutex);
    return CANCEL_BOOKING_ERROR_USER_NOT_LOGGED_IN;
  }
  pthread_mutex_unlock(&user_mutex);

  // 2. 좌석 번호 확인
  if (!is_number(request->data)) {
      return CANCEL_BOOKING_ERROR_SEAT_OUT_OF_RANGE;
  }
  int seat_id = atoi(request->data);
  if (seat_id < 1 || seat_id > NUM_SEATS) {
    return CANCEL_BOOKING_ERROR_SEAT_OUT_OF_RANGE;
  }

  // 3. 취소 처리
  Seat* seat = &seats[seat_id - 1];
  pthread_mutex_lock(&seat->mutex);

  if (seat->user_who_booked == NULL || 
      strcmp(seat->user_who_booked, request->username) != 0) {
    pthread_mutex_unlock(&seat->mutex);
    return CANCEL_BOOKING_ERROR_SEAT_NOT_BOOKED_BY_USER;
  }

  // 예약 해제
  free((void*)seat->user_who_booked);
  seat->user_who_booked = NULL;
  seat->amount_of_times_canceled++;

  pthread_mutex_unlock(&seat->mutex);

  return CANCEL_BOOKING_ERROR_SUCCESS;
}

int32_t handle_logout_request(const Request* request,
                              Response* response,
                              Users* users) {
  pthread_mutex_lock(&user_mutex);
  
  ssize_t uid = find_user(users, request->username);
  
  if (uid == -1) {
    pthread_mutex_unlock(&user_mutex);
    return LOGOUT_ERROR_USER_NOT_FOUND;
  }

  if (!users->array[uid].logged_in) {
    pthread_mutex_unlock(&user_mutex);
    return LOGOUT_ERROR_USER_NOT_LOGGED_IN;
  }

  users->array[uid].logged_in = false;
  pthread_mutex_unlock(&user_mutex);

  return LOGOUT_ERROR_SUCCESS;
}

int32_t handle_query_request(const Request* request,
                             Response* response,
                             Seat* seats) {
  if (request->data_size == 0 || request->data == NULL) {
      return QUERY_ERROR_NO_DATA;
  }

  // 1. 좌석 번호 확인
  if (!is_number(request->data)) {
      return QUERY_ERROR_SEAT_OUT_OF_RANGE;
  }
  int seat_id = atoi(request->data);
  if (seat_id < 1 || seat_id > NUM_SEATS) {
    return QUERY_ERROR_SEAT_OUT_OF_RANGE;
  }

  // 2. 좌석 정보 조회
  // 클라이언트는 Seat 구조체 전체를 바이너리로 받기를 원함 [cite: 265]
  Seat* seat_data = malloc(sizeof(Seat));
  Seat* target_seat = &seats[seat_id - 1];
  
  pthread_mutex_lock(&target_seat->mutex);
  
  // 구조체 복사 (Deep copy of user_who_booked is tricky here, 
  // but client mainly needs IDs and counts. 
  // user_who_booked is a pointer. Sending a pointer value to client is meaningless.
  // However, client code sets seat->user_who_booked = nullptr after memcpy.
  // So a shallow copy of the struct is fine for the counts and ID.)
  memcpy(seat_data, target_seat, sizeof(Seat));
  
  pthread_mutex_unlock(&target_seat->mutex);

  // 포인터 정보는 클라이언트에서 의미 없으므로 NULL 처리 (선택사항, 클라이언트가 어차피 덮어씀)
  seat_data->user_who_booked = NULL;
  // 뮤텍스도 클라이언트에서 쓸 일 없음

  response->data = (uint8_t*)seat_data;
  response->data_size = sizeof(Seat);

  return QUERY_ERROR_SUCCESS;
}

int32_t handle_request(const Request* request,
                       Response* response,
                       Users* users,
                       Seat* seats) {
  // 응답 데이터 초기화
  response->data = NULL;
  response->data_size = 0;
  
  int32_t ret_code;

  switch (request->action) {
    case ACTION_LOGIN:
      ret_code = handle_login_request(request, response, users);
      break;
    case ACTION_BOOK:
      ret_code = handle_book_request(request, response, users, seats);
      break;
    case ACTION_CONFIRM_BOOKING:
      ret_code = handle_confirm_booking_request(request, response, users, seats);
      break;
    case ACTION_CANCEL_BOOKING:
      ret_code = handle_cancel_booking_request(request, response, users, seats);
      break;
    case ACTION_LOGOUT:
      ret_code = handle_logout_request(request, response, users);
      break;
    case ACTION_QUERY:
      ret_code = handle_query_request(request, response, seats);
      break;
    case ACTION_TERMINATION:
      // 서버는 TERMINATION 액션을 받으면 안됨 (PDF 명세 [cite: 147])
      ret_code = -1; 
      break;
    default:
      // 정의되지 않은 액션 [cite: 93, 153]
      ret_code = -1;
      break;
  }
  
  response->code = ret_code;
  return 0; // handle_request return value is typically 0 on success logic flow
}
