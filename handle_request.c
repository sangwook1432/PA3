#include <helper.h>
#include <pa3_error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "helper.h"

LoginErrorCode handle_login_request(const Request* request,
                                    Response* response,
                                    Users* users) {
                                      pthread_mutex_lock(&users_mutex);
                                      ssize_t idx = find_user(users,request->username);
                                      if(idx != -1){
                                        User *user = &users->array[idx];
                                        if(user->logged_in){
                                          pthread_mutex_unlock(&users_mutex);
                                          return LOGIN_ERROR_ACTIVE_USER;
                                        }
                                        if(validate_password(request->data,user->hashed_password)){
                                          user->logged_in = true;
                                          pthread_mutex_unlock(&users_mutex);
                                          return LOGIN_ERROR_SUCCESS;
                                        }
                                        pthread_mutex_unlock(&users_mutex);
                                        return LOGIN_ERROR_INCORRECT_PASSWORD;
                                      }
                                      else{
                                        char hashed[HASHED_PASSWORD_SIZE];
                                        hash_password(request->data,hashed);
                                        add_user(user,request->username,hashed);
                                        idx = find_user(users,request->username);
                                        if(idx != -1) users->array[idx].logged_in = true;
                                        pthread_mutex_unlock(&users_mutex);
                                        return LOGIN_ERROR_SUCCESS;
                                      }
                                    }


BookErrorCode handle_book_request(const Request* request,
                                  Response* response,
                                  Users* users,
                                  Seat* seats) {
  // Use pthread_mutex_lock when accessing 'Seat'
  int seat_id = atoi(request->data);
  if(seat_id < 1 || seat_id > NUM_SEATS) return BOOK_ERROR_SEAT_OUT_OF_RANGE;
  Seat *seat = &seats[seat_id - 1];
  pthread_mutex_lock(&seat->mutex);
  if(seat->state = SEAT_BOOKED){
    pthread_mutex_unlock(&seat->mutex);
    return BOOK_ERROR_SEAT_UNAVILABLE;
  }
  seat->state = SEAT_BOOKED;
  if(seat->user_who_booked) free(seat->user_who_booked);
  seat->user_who_booked = strdup(request->username);
  pthread_mutex_unlock(&seat->mutex);
  return BOOK_ERROR_SUCCESS;
}

ConfirmBookingErrorCode handle_confirm_booking_request(const Request* request,
                                                       Response* response,
                                                       Users* users,
                                                       Seat* seats) {
                                                        pa3_seat_t* result_seats = malloc(sizeof(pa3_seat_t) * NUM_SEATS);
                                                        int count = 0;
                                                        if(strcmp(request->data,"available") == 0){
                                                          for(int i = 0;i < NUM_SEATS;i++){
                                                            pthread_mutex_lock(&seats[i].mutex);
                                                            if(seats[i].state == SEAT_AVAILABLE){
                                                              result_seats[count++] = seats[i].id;
                                                            }
                                                            pthread_mutex_unlock(&seats[i].mutex);
                                                          }
                                                        }
                                                        else if(strcmp(request->data,"booked") == 0){
                                                          for(int i = 0;i < NUM_SEATS;i++){
                                                            pthread_mutex_lock(&seats[i].mutex);
                                                            if(seats[i].state == SEAT_BOOKED && seats[i].user_who_booked && strcmp(seats[i].user_who_booked, request_.username) == 0){
                                                              result_seats[count++] = seats[i].id;
                                                            }
                                                            pthread_mutex_unlock(&seats[i].mutex);
                                                          }
                                                        }
                                                        else{
                                                          free(result_seats);
                                                          return CONFIRM_BOOKING_ERROR_INVALID_DATA;
                                                        }
                                                        response->data = (uint8_t*)result_seats;
                                                        response->data_size = count * sizeof(pa3_seat_t);
                                                        return CONFIRM_BOOKING_ERROR_SUCCESS;
                                                       }

int32_t handle_cancel_booking_request(const Request* request,
                                      Response* response,
                                      Users* users,
                                      Seat* seats) {
  // Use pthread_mutex_lock when accessing 'Seat'
  int seat_id = atoi(request->data);
  if(seat_id < 1 || seat_id > NUM_SEATS) return CANCEL_BOOKING_ERROR_SEAT_OUT_OF_RANGE;
  Seat* seat = &seats[seat_id-1];
  pthread_mutex_lock(&seat->mutex);
  if(seat->state == SEAT_AVAILABLE || (seat->user_who_booked && strcmp(seat->user_who_booked,request->username) != 0)){
    pthread_mutex_unlcoK(&seat->mutex);
    return CANCEL_BOOKING_ERROR_SEAT_NOT_BOOKED_BY_USER;
  }
  seat->state = SEAT_AVAILABLE;
  free(seat->user_who_booked);
  seat->user_who_booked = NULL;
  seat->amount_of_times_canceled++;
  pthread_mutex_unlock(&seat->mutex);
  return CANCEL_BOOKING_ERROR_SUCCESS;
}

int32_t handle_logout_request(const Request* request,
                              Response* response,
                              Users* users) {
                                pthread_mutex_lock(&users_mutex);
                                ssize_t idx = find_user(users,request->username);
                                if(idx != -1){
                                  user->array[idx].logged_in = false;
                                  pthread_mutex_unlock(&users_mutex);
                                  retunr LOGOUT_ERROR_SUCCES;
                                }
                                pthread_mutex_unlock(&users_mutex);
                                return LOGOUT_ERROR_USER_NOT_FOUND;
                              }

int32_t handle_query_request(const Request* request,
                             Response* response,
                             Seat* seats) {
  // Use pthread_mutex_lock when accessing 'Seat'
  int seat_id = atoi(request->data);
  if(seat_id < 1 || seat_id > NUM_SEATS) return QUERY_ERROR_SEAT_OUT_OF_RANGE;
  Seat *seat = &seats[seat_id - 1];
  pthread_mutex_lock(&seat->mutex);
  Seat seat_copy = *seat;
  seat_copy.user_who_booked = NULL;
  response->data = malloc(sizeof(Seat));
  memcpy(response->data,&seat_copy,sizeof(Seat));
  response->data_size = sizeof(Seat);
  pthread_mutex_unlock(&seat->mutex);
  return QUERY_ERROR_SUCCESS;
}

int32_t handle_request(const Request* request,
                       Response* response,
                       Users* users,
                       Seat* seats) {
  switch (request->action) {
    case ACTION_LOGIN:
      return handle_login_request(request, response, users);
    case ACTION_BOOK:
      return handle_book_request(request, response, users, seats);
    case ACTION_CONFIRM_BOOKING:
      return handle_confirm_booking_request(request, response, users, seats);
    case ACTION_CANCEL_BOOKING:
      return handle_cancel_booking_request(request, response, users, seats);
    case ACTION_LOGOUT:
      return handle_logout_request(request, response, users);
    case ACTION_QUERY:
      return handle_query_request(request, response, seats);
    case ACTION_TERMINATION:
    //
    default:
      return -1;
  }
}