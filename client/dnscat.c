#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "select_group.h"

#include "driver.h"
#include "driver_tcp.h"
#include "memory.h"
#include "packet.h"
#include "types.h"

typedef enum
{
  SESSION_STATE_NEW,
  SESSION_STATE_ESTABLISHED
} state_t;

typedef struct
{
  select_group_t *group;
  driver_t *driver;

  /* Session information */
  uint16_t session_id;
  state_t  session_state;
  uint16_t their_seq;
  uint16_t my_seq;

  buffer_t *incoming_data;
  buffer_t *outgoing_data;
} options_t;

static SELECT_RESPONSE_t stdin_callback(void *group, int socket, uint8_t *data, size_t length, char *addr, uint16_t port, void *param)
{
  options_t *options = (options_t*) param;

  buffer_add_bytes(options->outgoing_data, data, length);

  printf("Received %zd bytes from stdin, we have %zd bytes queued to send\n", length, buffer_get_remaining_bytes(options->outgoing_data));

  return SELECT_OK;
}

static SELECT_RESPONSE_t stdin_closed_callback(void *group, int socket, void *param)
{
  options_t *options = (options_t*) param;
  /* TODO: send a FIN */

  buffer_destroy(options->outgoing_data);
  buffer_destroy(options->incoming_data);
  select_group_destroy(options->group);
  driver_destroy(options->driver);
  printf("stdin closed\n");
  safe_free(options);

  print_memory();

  exit(0);
}

static void timeout_state_new(options_t *options)
{
  /* Send a syn */
  packet_t *packet = packet_create_syn(options->session_id, options->my_seq, 0);
  size_t   length;
  uint8_t *bytes = packet_to_bytes(packet, &length);

  driver_send(options->driver, bytes, length);

  safe_free(bytes);
  packet_destroy(packet);
}

static void timeout_state_established(options_t *options)
{
  uint8_t *out_data;
  size_t   out_length;
  uint8_t *in_data;
  size_t   in_length;

  out_data = buffer_read_remaining_bytes(options->outgoing_data, &out_length, options->driver->max_packet_size - 5); /* TODO: Magic number */

  /* TODO: handle session state properly */
  if(out_length > 0)
    driver_send(options->driver, out_data, out_length);
  safe_free(out_data);

  in_data = driver_recv(options->driver, &in_length, -1);
  if(in_data)
  {
    printf("Received %zd bytes: %s\n", in_length, in_data);
    safe_free(in_data);
  }
}

static SELECT_RESPONSE_t timeout(void *group, void *param)
{
  options_t *options = (options_t*) param;

  switch(options->session_state)
  {
    case SESSION_STATE_NEW:
      timeout_state_new(options);
      break;
    case SESSION_STATE_ESTABLISHED:
      timeout_state_established(options);
      break;
    default:
      printf("We ended up in an unknown state!\n");
      exit(1);
  }

  return SELECT_OK;
}

int main(int argc, const char *argv[])
{
  options_t *options = (options_t*)safe_malloc(sizeof(options_t));

  /* Set up the session */
  options->session_id = rand() % 0xFFFF;
  options->session_state = SESSION_STATE_NEW;
  options->their_seq = 0;
  options->my_seq = rand() % 0xFFFF;

  options->incoming_data = buffer_create(BO_BIG_ENDIAN);
  options->outgoing_data = buffer_create(BO_BIG_ENDIAN);

  /* Set up the STDIN socket */
  options->group = select_group_create();

#ifdef WIN32
  /* On Windows, the stdin_handle is quire complicated, and involves a sub-thread. */
  HANDLE stdin_handle = get_stdin_handle();
  select_group_add_pipe(group, -1, stdin_handle, (void*)options);
  select_set_recv(group, -1, stdin_callback);
  select_set_closed(group, -1, stdin_closed_callback);
#else
  /* On Linux, the stdin_handle is easy. */
  int stdin_handle = STDIN_FILENO;
  select_group_add_socket(options->group, stdin_handle, SOCKET_TYPE_STREAM, (void*)options);
  select_set_recv(options->group, stdin_handle, stdin_callback);
  select_set_closed(options->group, stdin_handle, stdin_closed_callback);

  /* Add the timeout function */
  select_set_timeout(options->group, timeout, (void*)options);
#endif

  /* Create the TCP driver */
  options->driver = driver_get_tcp("localhost", 2000, options->group);

  while(TRUE)
  {
    select_group_do_select(options->group, 1000);
  }

  return 0;
}
