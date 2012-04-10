/**
 * collectd - src/zeromq.c
 * Copyright (C) 2005-2010  Florian octo Forster
 * Copyright (C) 2009       Aman Gupta
 * Copyright (C) 2010       Julien Ammous
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at verplant.org>
 *   Aman Gupta <aman at tmm1.net>
 *   Julien Ammous
 **/

#include "collectd.h"
#include "common.h" /* auxiliary functions */
#include "plugin.h" /* plugin_register_*, plugin_dispatch_values */
#include "utils_cache.h"
#include "network.h"

/* for htons() */
#if HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif
#include <pthread.h>
#include <zmq.h>

#include "zeromq_borrowed.c"

struct cmq_socket_s
{
	void *socket;
	int type;
};
typedef struct cmq_socket_s cmq_socket_t;

static int cmq_threads_num = 1;
static void *cmq_context = NULL;

static pthread_t *receive_thread_ids = NULL;
static size_t     receive_thread_num = 0;
static int        sending_sockets_num = 0;

static _Bool thread_running = 1;

static void cmq_close_callback (void *socket) /* {{{ */
{
  if (socket != NULL)
    (void) zmq_close (socket);
} /* }}} void cmq_close_callback */

static void free_data (void *data, void *hint) /* {{{ */
{
  free (data);
} /* }}} void free_data */

static void *receive_thread (void *cmq_socket) /* {{{ */
{
  int status;
  char *data = NULL;
  size_t data_size;

  assert (cmq_socket != NULL);

  while (thread_running)
  {
    zmq_pollitem_t pollitem;
    zmq_msg_t msg;

    memset (&pollitem, 0, sizeof (pollitem));
    pollitem.socket = cmq_socket;
    pollitem.events = ZMQ_POLLIN;

    /* Check "thread_running" at least twice times per second. */
    status = zmq_poll (&pollitem, /* nitems = */ 1,
        /* timeout = */ 500000 /* us */);
    if (status < 0)
    {
      ERROR ("zeromq plugin: zmq_poll failed: %s", zmq_strerror (errno));
      continue;
    }
    else if (status == 0) /* timeout */
      continue;

    (void) zmq_msg_init (&msg);

    status = zmq_recv (cmq_socket, &msg, /* flags = */ 0);
    if (status != 0)
    {
      (void) zmq_msg_close (&msg);

      if ((errno == EAGAIN) || (errno == EINTR))
        continue;

      ERROR ("zeromq plugin: zmq_recv failed: %s", zmq_strerror (errno));
      break;
    }

    data = zmq_msg_data (&msg);
    data_size = zmq_msg_size (&msg);

    status = parse_packet (NULL, data, data_size,
        /* flags = */ 0,
        /* username = */ NULL);
    DEBUG("zeromq plugin: received data, parse returned %d", status);

    (void) zmq_msg_close (&msg);
  } /* while (thread_running) */

  DEBUG ("zeromq plugin: Receive thread is terminating.");
  (void) zmq_close (cmq_socket);
  
  return (NULL);
} /* }}} void *receive_thread */

#define PACKET_SIZE   512

static int write_notification (const notification_t *n, /* {{{ */
    __attribute__((unused)) user_data_t *user_data)
{
  char        buffer[PACKET_SIZE];
  char       *buffer_ptr = buffer;
  int         buffer_free = sizeof (buffer);
  int         status;
  zmq_msg_t   msg;
  
  void *cmq_socket = user_data->data;

  memset (buffer, '\0', sizeof (buffer));


  status = write_part_number (&buffer_ptr, &buffer_free, TYPE_TIME, (uint64_t) n->time);
  if (status != 0)
    return (-1);

  status = write_part_number (&buffer_ptr, &buffer_free, TYPE_SEVERITY, (uint64_t) n->severity);
  if (status != 0)
    return (-1);

  if (strlen (n->host) > 0)
  {
    status = write_part_string (&buffer_ptr, &buffer_free, TYPE_HOST, n->host, strlen (n->host));
    if (status != 0)
      return (-1);
  }

  if (strlen (n->plugin) > 0)
  {
    status = write_part_string (&buffer_ptr, &buffer_free, TYPE_PLUGIN, n->plugin, strlen (n->plugin));
    if (status != 0)
      return (-1);
  }

  if (strlen (n->plugin_instance) > 0)
  {
    status = write_part_string (&buffer_ptr, &buffer_free, TYPE_PLUGIN_INSTANCE, n->plugin_instance,
      strlen (n->plugin_instance));
    if (status != 0)
      return (-1);
  }

  if (strlen (n->type) > 0)
  {
    status = write_part_string (&buffer_ptr, &buffer_free, TYPE_TYPE, n->type, strlen (n->type));
    if (status != 0)
      return (-1);
  }

  if (strlen (n->type_instance) > 0)
  {
    status = write_part_string (&buffer_ptr, &buffer_free, TYPE_TYPE_INSTANCE, n->type_instance,
      strlen (n->type_instance));
    if (status != 0)
      return (-1);
  }

  status = write_part_string (&buffer_ptr, &buffer_free, TYPE_MESSAGE, n->message, strlen (n->message));
  if (status != 0)
    return (-1);
  
  // zeromq will free the memory when needed by calling the free_data function
  if( zmq_msg_init_data(&msg, buffer, sizeof(buffer) - buffer_free, free_data, NULL) != 0 ) {
    ERROR("zmq_msg_init : %s", zmq_strerror(errno));
    return 1;
  }

  // try to send the message
  if( zmq_send(cmq_socket, &msg, /* flags = */ 0) != 0 ) {
    if( errno == EAGAIN ) {
      WARNING("ZeroMQ: Cannot send message, queue is full");
    }
    else {
      ERROR("zmq_send : %s", zmq_strerror(errno));
      return 1;
    }
  }
  
  return 0;
} /* }}} int write_notification */

static int write_value (const data_set_t *ds, /* {{{ */
    const value_list_t *vl,
    user_data_t *user_data)
{
  void *cmq_socket = user_data->data;

  zmq_msg_t msg;
  char      *send_buffer;
  int       send_buffer_size = PACKET_SIZE, real_size;

  send_buffer = malloc(PACKET_SIZE);
  if( send_buffer == NULL ) {
    ERROR("Unable to allocate memory for send_buffer, aborting write");
    return 1;
  }

  // empty buffer
  memset(send_buffer, 0, PACKET_SIZE);

  real_size = add_to_buffer(send_buffer, send_buffer_size, &send_buffer_vl, ds, vl);

  // zeromq will free the memory when needed by calling the free_data function
  if( zmq_msg_init_data(&msg, send_buffer, real_size, free_data, NULL) != 0 ) {
    ERROR("zmq_msg_init : %s", zmq_strerror(errno));
    return 1;
  }

  // try to send the message
  if( zmq_send(cmq_socket, &msg, ZMQ_NOBLOCK) != 0 ) {
    if( errno == EAGAIN ) {
      WARNING("ZeroMQ: Unable to queue message, queue may be full");
      return -1;
    }
    else {
      ERROR("zmq_send : %s", zmq_strerror(errno));
      return -1;
    }
  }

  DEBUG("ZeroMQ: data sent");

  return 0;
} /* }}} int write_value */

static int cmq_config_mode (oconfig_item_t *ci) /* {{{ */
{
  char buffer[64] = "";
  int status;

  status = cf_util_get_string_buffer (ci, buffer, sizeof (buffer));
  if (status != 0)
    return (-1);

  if (strcasecmp ("Publish", buffer) == 0)
    return (ZMQ_PUB);
  else if (strcasecmp ("Subscribe", buffer) == 0)
    return (ZMQ_SUB);
  else if (strcasecmp ("Push", buffer) == 0)
    return (ZMQ_PUSH);
  else if (strcasecmp ("Pull", buffer) == 0)
    return (ZMQ_PULL);
  
  ERROR ("zeromq plugin: Unrecognized communication pattern: \"%s\"",
      buffer);
  return (-1);
} /* }}} int cmq_config_mode */

static int cmq_config_socket (oconfig_item_t *ci) /* {{{ */
{
  int type;
  int status;
  int i;
  int endpoints_num;
  void *cmq_socket;

  type = cmq_config_mode (ci);
  if (type < 0)
    return (-1);

  if (cmq_context == NULL)
  {
    cmq_context = zmq_init (cmq_threads_num);
    if (cmq_context == NULL)
    {
      ERROR ("zeromq plugin: Initializing ZeroMQ failed: %s",
          zmq_strerror (errno));
      return (-1);
    }
    
    INFO("ZeroMQ: Using %d threads", cmq_threads_num);
  }

  /* Create a new socket */
  cmq_socket = zmq_socket (cmq_context, type);
  if (cmq_socket == NULL)
  {
    ERROR ("zeromq plugin: zmq_socket failed: %s",
        zmq_strerror (errno));
    return (-1);
  }

  if (type == ZMQ_SUB)
  {
    /* Subscribe to all messages */
    status = zmq_setsockopt (cmq_socket, ZMQ_SUBSCRIBE,
        /* prefix = */ "", /* prefix length = */ 0);
    if (status != 0)
    {
      ERROR ("zeromq plugin: zmq_setsockopt (ZMQ_SUBSCRIBE) failed: %s",
          zmq_strerror (errno));
      (void) zmq_close (cmq_socket);
      return (-1);
    }
  }

  /* Iterate over all children and do all the binds and connects requested. */
  endpoints_num = 0;
  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp ("Endpoint", child->key) == 0)
    {
      char *value = NULL;

      status = cf_util_get_string (child, &value);
      if (status != 0)
        continue;

      if ((type == ZMQ_SUB) || (type == ZMQ_PULL))
      {
        DEBUG("Binding to %s", value);
        status = zmq_bind (cmq_socket, value);
        if (status != 0)
        {
          ERROR ("zeromq plugin: zmq_bind (\"%s\") failed: %s",
              value, zmq_strerror (errno));
          sfree (value);
          continue;
        }
      }
      else if ((type == ZMQ_PUB) || (type == ZMQ_PUSH))
      {
        DEBUG("Connecting to %s", value);
        status = zmq_connect (cmq_socket, value);
        if (status != 0)
        {
          ERROR ("zeromq plugin: zmq_connect (\"%s\") failed: %s",
              value, zmq_strerror (errno));
          sfree (value);
          continue;
        }
      }
      else
      {
        assert (23 == 42);
      }
      
      sfree (value);

      endpoints_num++;
      continue;
    } /* Endpoint */
    else if( strcasecmp("HWM", child->key) == 0 )
    {
      int tmp;
      uint64_t hwm;
      
      status = cf_util_get_int(child, &tmp);
      if( status != 0 )
        continue;
      
      hwm = (uint64_t) tmp;
      
      status = zmq_setsockopt (cmq_socket, ZMQ_HWM, &hwm, sizeof(hwm));
      if (status != 0) {
        ERROR ("zeromq plugin: zmq_setsockopt (ZMQ_HWM) failed: %s", zmq_strerror (errno));
        (void) zmq_close (cmq_socket);
        return (-1);
      }
      
      continue;
    } /* HWM */
    else
    {
      ERROR ("zeromq plugin: The \"%s\" config option is now allowed here.",
          child->key);
    }
  } /* for (i = 0; i < ci->children_num; i++) */

  if (endpoints_num == 0)
  {
    ERROR ("zeromq plugin: No (valid) \"Endpoint\" option was found in this "
        "\"Socket\" block.");
    (void) zmq_close (cmq_socket);
    return (-1);
  }

  /* If this is a receiving socket, create a new receive thread */
  if ((type == ZMQ_SUB) || (type == ZMQ_PULL))
  {
    pthread_t *thread_ptr;

    thread_ptr = realloc (receive_thread_ids,
        sizeof (*receive_thread_ids) * (receive_thread_num + 1));
    if (thread_ptr == NULL)
    {
      ERROR ("zeromq plugin: realloc failed.");
      return (-1);
    }
    receive_thread_ids = thread_ptr;
    thread_ptr = receive_thread_ids + receive_thread_num;

    status = pthread_create (thread_ptr,
        /* attr = */ NULL,
        /* func = */ receive_thread,
        /* args = */ cmq_socket);
    if (status != 0)
    {
      char errbuf[1024];
      ERROR ("zeromq plugin: pthread_create failed: %s",
          sstrerror (errno, errbuf, sizeof (errbuf)));
      (void) zmq_close (cmq_socket);
      return (-1);
    }

    receive_thread_num++;
  }

  /* If this is a sending socket, register a new write function */
  else if ((type == ZMQ_PUB) || (type == ZMQ_PUSH))
  {
    user_data_t ud = { NULL, NULL };
    char name[20];

    ud.data = cmq_socket;
    ud.free_func = cmq_close_callback;

    ssnprintf (name, sizeof (name), "zeromq/%i", sending_sockets_num);
    sending_sockets_num++;
    
    plugin_register_write (name, write_value, &ud);
    
    ssnprintf (name, sizeof (name), "zeromq/%i/notif", sending_sockets_num);
    
    plugin_register_notification (name, write_notification, &ud);
  }

  return (0);
} /* }}} int cmq_config_socket */

/*
 * Config schema:
 *
 * <Plugin "zeromq">
 *   Threads 2
 *
 *   <Socket Publish>
 *     HWM 300
 *     Endpoint "tcp://localhost:6666"
 *   </Socket>
 *   <Socket Subscribe>
 *     Endpoint "tcp://eth0:6666"
 *     Endpoint "tcp://collectd.example.com:6666"
 *   </Socket>
 * </Plugin>
 */
static int cmq_config (oconfig_item_t *ci) /* {{{ */
{
  int status;
  int i;
  
  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp ("Socket", child->key) == 0)
      status = cmq_config_socket (child);
    else if (strcasecmp ("Threads", child->key) == 0)
    {
      int tmp = 0;
      status = cf_util_get_int (child, &tmp);
      if ((status == 0) && (tmp >= 1))
        cmq_threads_num = tmp;
    }
    else
    {
      WARNING ("zeromq plugin: The \"%s\" config option is not allowed here.",
          child->key);
    }
  }

  return (0);
} /* }}} int cmq_config */

static int plugin_init (void) /* {{{ */
{
  int major, minor, patch;
  zmq_version (&major, &minor, &patch);
  INFO("ZeroMQ plugin loaded (zeromq v%d.%d.%d).", major, minor, patch);
  return 0;
} /* }}} int plugin_init */

static int cmq_shutdown (void) /* {{{ */
{
  size_t i;

  if (cmq_context == NULL)
    return (0);

  /* Signal thread to shut down */
  thread_running = 0;
    
  DEBUG ("ZeroMQ plugin: Waiting for %zu receive thread(s) to shut down.",
      receive_thread_num);
    
  for (i = 0; i < receive_thread_num; i++)
    pthread_join (receive_thread_ids[i], /* return ptr = */ NULL);

  if (zmq_term (cmq_context) != 0)
  {
    ERROR ("ZeroMQ plugin: zmq_term failed: %s", zmq_strerror (errno));
    return 1;
  }
    
  return 0;
} /* }}} int cmq_shutdown */

void module_register (void)
{
  plugin_register_complex_config("zeromq", cmq_config);
  plugin_register_init("zeromq", plugin_init);
  plugin_register_shutdown ("zeromq", cmq_shutdown);
}

/* vim: set sw=2 sts=2 et fdm=marker : */
