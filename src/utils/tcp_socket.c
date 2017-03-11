#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "assert.h"
#include "config.h"
#include "tcp_socket.h"

void ne_tcp_socket_init(ne_loop_t *loop, ne_tcp_socket_t *socket) {
  memset(socket, 0, sizeof(ne_tcp_socket_t));

  socket->status = INVALID;

  uv_tcp_init(loop, &socket->handle.tcp);
  socket->handle.tcp.data = socket;

  uv_timer_init(loop, &socket->timeout_timer);
  socket->timeout_timer.data = socket;
  socket->timeout = SOCKET_TIMEOUT;
}

/* MARK: Timeout related. */
void __ne_tcp_socket_timeout_cb(uv_timer_t *handle) {
  ne_tcp_socket_t *socket = (ne_tcp_socket_t *)handle->data;
  if (socket->on_error)
    socket->on_error(socket, NE_TCP_ETIMEOUT);
  ne_tcp_socket_close(socket);
}

void __ne_tcp_socket_timer_reset(ne_tcp_socket_t *socket) {
  if (socket->timeout > 0)
    uv_timer_again(&socket->timeout_timer);
}

void __ne_tcp_socket_timer_start(ne_tcp_socket_t *socket) {
  if (socket->timeout > 0)
    uv_timer_start(&socket->timeout_timer, __ne_tcp_socket_timeout_cb,
                   socket->timeout, socket->timeout);
}

void __ne_tcp_socket_timer_stop(ne_tcp_socket_t *socket) {
  uv_timer_stop(&socket->timeout_timer);
}

void __ne_tcp_socket_check_close(ne_tcp_socket_t *socket) {
  if (socket->timer_closed && socket->socket_closed) {
    socket->status = CLOSED;
    if (socket->on_close)
      socket->on_close(socket);
  }
}

void __ne_tcp_socket_timer_close_cb(uv_handle_t *handle) {
  ne_tcp_socket_t *socket = (ne_tcp_socket_t *)handle->data;
  socket->timer_closed = true;
  __ne_tcp_socket_check_close(socket);
}

void __ne_tcp_socket_socket_close_cb(uv_handle_t *handle) {
  ne_tcp_socket_t *socket = (ne_tcp_socket_t *)handle->data;
  socket->socket_closed = true;
  __ne_tcp_socket_check_close(socket);
}

void ne_tcp_socket_close(ne_tcp_socket_t *socket) {
  socket->status = CLOSING;
  uv_close((uv_handle_t *)&socket->timeout_timer,
           __ne_tcp_socket_timer_close_cb);
  uv_close(&socket->handle.handle, __ne_tcp_socket_socket_close_cb);
}

/* Return true when the error is handled and the caller should just return.
   Return false when there is no error or the caller should handle it. */
bool __ne_tcp_socket_handle_error(ne_tcp_socket_t *socket, int status) {
  if (status >= 0)
    return false;

  /* Closing socket should only expect to receive closed callback. */
  if (socket->status >= CLOSING)
    return true;

  switch (status) {
  case UV_EOF:
    return false;
  case UV_ETIMEDOUT:
    if (socket->on_error)
      socket->on_error(socket, NE_TCP_ETIMEOUT);
    ne_tcp_socket_close(socket);
    return true;
  case UV_ECONNRESET:
    if (socket->on_error)
      socket->on_error(socket, NE_TCP_ERST);
    ne_tcp_socket_close(socket);
    return true;
  case UV_ENETUNREACH:
    if (socket->on_error)
      socket->on_error(socket, NE_TCP_ENETUNREACH);
    ne_tcp_socket_close(socket);
    return true;
  case UV_ECANCELED:
    /* This should only happen when the socket is closing, which is already
     * checked. */
    NE_ASSERT(false);
  }
  /* Catch all other errors as of now so we know what we should handle but
   * missing. */
  NE_ASSERT(false);
  return false;
}

void ne_tcp_socket_bind(ne_tcp_socket_t *socket, struct sockaddr *addr) {
  /* The addr in use error is always delayed. */
  NE_ASSERT(!uv_tcp_bind(&socket->handle.tcp, addr, 0));
}

void __ne_tcp_socket_accepted(ne_tcp_socket_t *socket) {
  socket->status = CONNECTED;
  __ne_tcp_socket_timer_start(socket);
}

void __ne_tcp_socket_connection_cb(uv_stream_t *listen_stream, int status) {
  NE_ASSERT(status == 0);

  ne_tcp_socket_t *socket = (ne_tcp_socket_t *)listen_stream->data;
  ne_tcp_socket_t *accepted_socket = socket->connection_alloc(socket);
  ne_tcp_socket_init(socket->handle.tcp.loop, accepted_socket);

  NE_ASSERT(uv_accept(listen_stream, &accepted_socket->handle.stream));
  __ne_tcp_socket_accepted(accepted_socket);

  socket->on_connection(socket, accepted_socket);
}

void ne_tcp_socket_listen(ne_tcp_socket_t *socket, int backlog) {
  NE_ASSERT(uv_listen(&socket->handle.stream, backlog,
                      __ne_tcp_socket_connection_cb));
}

void __ne_tcp_socket_connect_cb(uv_connect_t *req, int status) {
  ne_tcp_socket_t *socket = (ne_tcp_socket_t *)req->handle->data;

  if (__ne_tcp_socket_handle_error(socket, status))
    return;

  NE_ASSERT(status == 0);

  __ne_tcp_socket_timer_reset(socket);

  socket->on_connect(socket);
}

ne_tcp_socket_connect_err ne_tcp_socket_connect(ne_tcp_socket_t *socket,
                                                struct sockaddr *addr) {
  int r = uv_tcp_connect(&socket->connect_req, &socket->handle.tcp, addr,
                         __ne_tcp_socket_connect_cb);
  if (!r) {
    socket->status = CONNECTING;
    __ne_tcp_socket_timer_start(socket);
    return NE_TCP_CNOERR;
  }

  switch (r) {
  case UV_EADDRINUSE:
    return NE_TCP_CEADDRINUSE;
  case UV_ENETUNREACH:
    return NE_TCP_CENETUNREACH;
  default:
    NE_ASSERT(false);
    return r;
  }
}

/* MARK: Read related */
void ne_tcp_socket_read_stop(ne_tcp_socket_t *socket) {
  socket->reading = false;
  uv_read_stop(&socket->handle.stream);
}

void __ne_tcp_socket_read_cb(uv_stream_t *stream, ssize_t nread,
                             const uv_buf_t *buf) {
  ne_tcp_socket_t *socket = (ne_tcp_socket_t *)stream->data;

  if (__ne_tcp_socket_handle_error(socket, nread)) {
    socket->reading = false;
    if (socket->on_read)
      socket->on_read(socket, NE_TCP_RERR, buf);
    return;
  }

  if (nread >= 0)
    __ne_tcp_socket_timer_reset(socket);

  if (nread == UV_EOF) {
    socket->reading = false;
    socket->receiveEOF = true;
    ne_tcp_socket_read_stop(socket);
    if (socket->on_read)
      socket->on_read(socket, NE_TCP_REOF, buf);
    return;
  } else {
    NE_ASSERT(nread >= 0);
    if (socket->on_read)
      socket->on_read(socket, nread, buf);
  }
}

void __ne_tcp_socket_alloc_cb(uv_handle_t *handle,
                              size_t UNUSED(suggested_size), uv_buf_t *buf) {
  ne_tcp_socket_t *socket = (ne_tcp_socket_t *)handle->data;

  socket->alloc_cb(socket, buf);
}

int ne_tcp_socket_read_start(ne_tcp_socket_t *socket) {
  NE_ASSERT(socket->status <= SHUT_DOWN);
  NE_ASSERT(!socket->receiveEOF);
  socket->reading = true;
  return uv_read_start(&socket->handle.stream, __ne_tcp_socket_alloc_cb,
                       __ne_tcp_socket_read_cb);
}

void __ne_tcp_socket_write_cb(uv_write_t *req, int status) {
  ne_tcp_socket_t *socket = (ne_tcp_socket_t *)req->handle->data;
  if (__ne_tcp_socket_handle_error(socket, status)) {
    socket->on_write(socket, NE_TCP_WERR);
    return;
  }

  NE_ASSERT(status == 0);
  socket->on_write(socket, NE_TCP_WNOERR);
}

void ne_tcp_socket_write(ne_tcp_socket_t *socket) {
  NE_ASSERT(socket->status < SHUTTING_DOWN);
  uv_write(&socket->write_req, &socket->handle.stream, &socket->write_buf, 1,
           __ne_tcp_socket_write_cb);
}

void __ne_tcp_socket_shutdown_cb(uv_shutdown_t *req, int status) {
  ne_tcp_socket_t *socket = (ne_tcp_socket_t *)req->handle->data;
  if (__ne_tcp_socket_handle_error(socket, status))
    return;

  NE_ASSERT(status == 0);
  socket->status = SHUT_DOWN;
  socket->on_shutdown(socket);
}
void ne_tcp_socket_shutdown(ne_tcp_socket_t *socket) {
  NE_ASSERT(socket->status == CONNECTING || socket->status == CONNECTED);
  socket->status = SHUTTING_DOWN;
  uv_shutdown(&socket->shutdown_req, &socket->handle.stream,
              __ne_tcp_socket_shutdown_cb);
}

void ne_tcp_socket_deinit(ne_tcp_socket_t *socket) {
  NE_ASSERT(socket->status == CLOSED);
}