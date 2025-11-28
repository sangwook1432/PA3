#include <helper.h>
#include <pa3_error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "helper.h"

// Users 배열 접근을 보호하기 위한 정적 뮤텍스
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
  (void)response; // Unused parameter

  if (request->data_size == 0 || request->data == NULL) {
    return LOGIN_ERROR_NO_PASSWORD; 
  }

  char* username = request->username;
  char* password = request->data;

  pthread_mutex_lock(&user_mutex);

  ssize_t uid = find_user(users, username);

  if (uid == -1) {
    // 신규 유저 등록
    char hashed_password[HASHED_PASSWORD_SIZE];
    hash_password(password, hashed_password);
    size_t new_uid = add_user(users, username, hashed_password);
    users->array[new_uid].logged_in = true;
    pthread_mutex_unlock(&user_mutex);
    return LOGIN_ERROR_SUCCESS;
  } else {
    // 기존 유저 로그인
    User* user = &users->array[uid];

    if (user->logged_in) {
      pthread_mutex_unlock(&user_mutex);
      return LOGIN_ERROR_ACTIVE_USER;
    }

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
  (void)response;

  // 1. 데이터 유효성 검사
  if (request->data_size == 0 || request->data == NULL) {
    return BOOK_ERROR_NO_DATA;
  }

  // 2. 유저 로그인 상태 확인
  pthread_mutex_lock(&user_mutex);
  ssize_t uid = find_user(users, request->username);
  if (uid == -1 || !users->array[uid].logged_in) {
    pthread_mutex_unlock(&user_mutex);
    return BOOK_ERROR_USER_NOT_LOGGED_IN;
  }
  pthread_mutex_unlock(&user_mutex);

  // 3. 좌석 번호 파싱
  if (!is_number(request->data)) {
      return BOOK_ERROR_SEAT_OUT_OF_RANGE;
  }
  int seat_id = atoi(request->data);
  if (seat_id < 1 || seat_id > NUM_SEATS) {
    return BOOK_ERROR_SEAT_OUT_OF_RANGE;
  }

  // 4. 좌석 예약
  Seat* seat = &seats[seat_id - 1];
  pthread_mutex_lock(&seat->mutex);

  if (seat->user_who_booked != NULL) {
    pthread_mutex_unlock(&seat->mutex);
    return BOOK_ERROR_SEAT_UNAVAILABLE;
  }

  if (seat->user_who_booked) free((void*)seat->user_who_booked); // const casting
  seat->user_who_booked = strdup(request->username);
  seat->amount_of_times_booked++;
  
  pthread_mutex_unlock(&seat->mutex);

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

  // 2. 요청 타입 확인
  bool check_available = false;
  if (strcmp(request->data, "available") == 0) {
    check_available = true;
  } else if (strcmp(request->data, "booked") == 0) {
    check_available = false;
  } else {
    return CONFIRM_BOOKING_ERROR_INVALID_DATA;
  }

  // 3. 좌석 정보 수집 (pa3_seat_t 타입 사용 권장)
  pa3_seat_t* result_array = malloc(sizeof(pa3_seat_t) * NUM_SEATS);
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
  response->data_size = count * sizeof(pa3_seat_t);
  
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
  (void)response;

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

  free((void*)seat->user_who_booked); // const casting
  seat->user_who_booked = NULL;
  seat->amount_of_times_canceled++;

  pthread_mutex_unlock(&seat->mutex);

  return CANCEL_BOOKING_ERROR_SUCCESS;
}

int32_t handle_logout_request(const Request* request,
                              Response* response,
                              Users* users) {
  (void)response;

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
  Seat* seat_data = malloc(sizeof(Seat));
  Seat* target_seat = &seats[seat_id - 1];
  
  pthread_mutex_lock(&target_seat->mutex);
  memcpy(seat_data, target_seat, sizeof(Seat));
  pthread_mutex_unlock(&target_seat->mutex);

  seat_data->user_who_booked = NULL; 

  response->data = (uint8_t*)seat_data;
  response->data_size = sizeof(Seat);

  return QUERY_ERROR_SUCCESS;
}

int32_t handle_request(const Request* request,
                       Response* response,
                       Users* users,
                       Seat* seats) {
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
      ret_code = -1; 
      break;
    default:
      ret_code = -1;
      break;
  }
  
  // [중요 수정] pa3_server.c와의 호환성을 위해 리턴값 수정
  response->code = ret_code;
  return ret_code; // pa3_server.c가 이 리턴값을 쓸 수도 있으므로 일치시킴
}
  
  response->code = ret_code;
  return 0; // handle_request return value is typically 0 on success logic flow
}
