/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>

static void uv__pipe_accept(uv_loop_t* loop, uv__io_t* w, unsigned int events);


int uv_pipe_init(uv_loop_t* loop, uv_pipe_t* handle, int flags) {
  uv__stream_init(loop, (uv_stream_t*)handle, UV_NAMED_PIPE);
  handle->shutdown_req = NULL;
  handle->connect_req = NULL;
  handle->pipe_fname = NULL;
    
  handle->flags |= ((flags&UV_PIPE_IPC)?UV_HANDLE_PIPE_IPC:0)|
          ((flags&UV_PIPE_SPAWN_SAFE)?UV_HANDLE_PIPE_SPAWN_SAFE:0)|
          ((flags&UV_PIPE_READABLE)?UV_STREAM_READABLE:0)|
          ((flags&UV_PIPE_WRITEABLE)?UV_STREAM_WRITABLE:0);
  return 0;
}


int uv_pipe_bind(uv_pipe_t* handle, const char* name) {
  struct sockaddr_un saddr;
  const char* pipe_fname;
  int sockfd;
  int bound;
  int err;

  pipe_fname = NULL;
  sockfd = -1;
  bound = 0;
  err = -EINVAL;

  /* Already bound? */
  if (uv__stream_fd(handle) >= 0)
    return -EINVAL;

  /* Make a copy of the file name, it outlives this function's scope. */
  pipe_fname = strdup(name);
  if (pipe_fname == NULL) {
    err = -ENOMEM;
    goto out;
  }

  /* We've got a copy, don't touch the original any more. */
  name = NULL;

  err = uv__socket(AF_UNIX, SOCK_STREAM, 0);
  if (err < 0)
    goto out;
  sockfd = err;

  memset(&saddr, 0, sizeof saddr);
  uv_strlcpy(saddr.sun_path, pipe_fname, sizeof(saddr.sun_path));
  saddr.sun_family = AF_UNIX;

  if (bind(sockfd, (struct sockaddr*)&saddr, sizeof saddr)) {
    err = -errno;
    /* Convert ENOENT to EACCES for compatibility with Windows. */
    if (err == -ENOENT)
      err = -EACCES;
    goto out;
  }
  bound = 1;

  /* Success. */
  handle->pipe_fname = pipe_fname; /* Is a strdup'ed copy. */
  handle->io_watcher.fd = sockfd;
  return 0;

out:
  if (bound) {
    /* unlink() before close() to avoid races. */
    assert(pipe_fname != NULL);
    unlink(pipe_fname);
  }
  close(sockfd);
  free((void*)pipe_fname);
  return err;
}

int uv_pipe_link(uv_pipe_t *read, uv_pipe_t *write) {
  int err;
  int fds[2];

  assert(read->loop==write->loop);
  assert(read->flags&UV_STREAM_READABLE);
  assert(write->flags&UV_STREAM_WRITABLE);
  assert(!(write->flags&read->flags&UV_HANDLE_PIPE_IPC));

#ifdef SOCK_NONBLOCK
  int fl;

  fl = SOCK_CLOEXEC;

  if (~((read->flags|write->flags)&UV_PIPE_SPAWN_SAFE)) {
    if (socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|fl, 0, fds) == 0)
        goto open_fds;

    if (errno != EINVAL)
      goto pipe_error;
    /* errno == EINVAL so maybe the kernel headers lied about
     * the availability of SOCK_NONBLOCK. This can happen if people
     * build libuv against newer kernel headers than the kernel
     * they actually run the software on.
     */
  }
#endif

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
    goto pipe_error;

  uv__cloexec(fds[0], 1);
  uv__cloexec(fds[1], 1);

  if (~(read->flags & UV_PIPE_SPAWN_SAFE))
    uv__nonblock(fds[0], 1);
  if (~(write->flags & UV_PIPE_SPAWN_SAFE))
    uv__nonblock(fds[1], 1);

open_fds: 
  
  err = uv__stream_open((uv_stream_t*)read, fds[0], 0);
  if (err) {
      close(fds[0]);
      close(fds[1]);
      goto pipe_error;
  }

  err = uv__stream_open((uv_stream_t*)write, fds[1], 0);
  if (err) {
      uv_pipe_close_sync(read);
      close(fds[0]);
      close(fds[1]);
      goto pipe_error;
  }

  return 0;
  
pipe_error:
  return -1;
}

void uv_pipe_close_sync(uv_pipe_t *pipe) {
    uv__stream_close((uv_stream_t*)pipe); /* TODO: ??? */
    pipe->close_cb=0;
    pipe->flags |= UV_CLOSING;
    uv__finish_close((uv_handle_t*)pipe);
}

int uv_pipe_listen(uv_pipe_t* handle, int backlog, uv_connection_cb cb) {
  if (uv__stream_fd(handle) == -1)
    return -EINVAL;

  if (listen(uv__stream_fd(handle), backlog))
    return -errno;

  handle->connection_cb = cb;
  handle->io_watcher.cb = uv__pipe_accept;
  uv__io_start(handle->loop, &handle->io_watcher, UV__POLLIN);
  return 0;
}


void uv__pipe_close(uv_pipe_t* handle) {
  if (handle->pipe_fname) {
    /*
     * Unlink the file system entity before closing the file descriptor.
     * Doing it the other way around introduces a race where our process
     * unlinks a socket with the same name that's just been created by
     * another thread or process.
     */
    unlink(handle->pipe_fname);
    free((void*)handle->pipe_fname);
    handle->pipe_fname = NULL;
  }

  uv__stream_close((uv_stream_t*)handle);
}


int uv_pipe_open(uv_pipe_t* handle, uv_file fd) {
#if defined(__APPLE__)
  int err;

  err = uv__stream_try_select((uv_stream_t*) handle, &fd);
  if (err)
    return err;
#endif /* defined(__APPLE__) */

  return uv__stream_open((uv_stream_t*)handle,
                         fd,
                         0);
}


void uv_pipe_connect(uv_connect_t* req,
                    uv_pipe_t* handle,
                    const char* name,
                    uv_connect_cb cb) {
  struct sockaddr_un saddr;
  int new_sock;
  int err;
  int r;

  new_sock = (uv__stream_fd(handle) == -1);
  err = -EINVAL;

  if (new_sock) {
    err = uv__socket(AF_UNIX, SOCK_STREAM, 0);
    if (err < 0)
      goto out;
    handle->io_watcher.fd = err;
  }

  memset(&saddr, 0, sizeof saddr);
  uv_strlcpy(saddr.sun_path, name, sizeof(saddr.sun_path));
  saddr.sun_family = AF_UNIX;

  do {
    r = connect(uv__stream_fd(handle),
                (struct sockaddr*)&saddr, sizeof saddr);
  }
  while (r == -1 && errno == EINTR);

  if (r == -1 && errno != EINPROGRESS) {
    err = -errno;
    goto out;
  }

  err = 0;
  if (new_sock) {
    err = uv__stream_open((uv_stream_t*)handle,
                          uv__stream_fd(handle),
                          UV_STREAM_READABLE | UV_STREAM_WRITABLE);
  }

  if (err == 0)
    uv__io_start(handle->loop, &handle->io_watcher, UV__POLLIN | UV__POLLOUT);

out:
  handle->delayed_error = err;
  handle->connect_req = req;

  uv__req_init(handle->loop, req, UV_CONNECT);
  req->handle = (uv_stream_t*)handle;
  req->cb = cb;
  QUEUE_INIT(&req->queue);

  /* Force callback to run on next tick in case of error. */
  if (err)
    uv__io_feed(handle->loop, &handle->io_watcher);

  /* Mimic the Windows pipe implementation, always
   * return 0 and let the callback handle errors.
   */
}


/* TODO merge with uv__server_io()? */
static void uv__pipe_accept(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  uv_pipe_t* pipe;
  int sockfd;

  pipe = container_of(w, uv_pipe_t, io_watcher);
  assert(pipe->type == UV_NAMED_PIPE);

  sockfd = uv__accept(uv__stream_fd(pipe));
  if (sockfd == -1) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      pipe->connection_cb((uv_stream_t*)pipe, -errno);
    return;
  }

  pipe->accepted_fd = sockfd;
  pipe->connection_cb((uv_stream_t*)pipe, 0);
  if (pipe->accepted_fd == sockfd) {
    /* The user hasn't called uv_accept() yet */
    uv__io_stop(pipe->loop, &pipe->io_watcher, UV__POLLIN);
  }
}


void uv_pipe_pending_instances(uv_pipe_t* handle, int count) {
}
