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

#include <assert.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

#include "uv.h"
#include "internal.h"
#include "handle-inl.h"


/*
 * The `child_stdio_buffer` buffer has the following layout:
 *   int number_of_fds
 *   unsigned char crt_flags[number_of_fds]
 *   HANDLE os_handle[number_of_fds]
 */
#define CHILD_STDIO_SIZE(count)                     \
    (sizeof(int) +                                  \
     sizeof(unsigned char) * (count) +              \
     sizeof(uintptr_t) * (count))

#define CHILD_STDIO_COUNT(buffer)                   \
    *((unsigned int*) (buffer))

#define CHILD_STDIO_CRT_FLAGS(buffer, fd)           \
    *((unsigned char*) (buffer) + sizeof(int) + fd)

#define CHILD_STDIO_HANDLE(buffer, fd)              \
    *((HANDLE*) ((unsigned char*) (buffer) +        \
                 sizeof(int) +                      \
                 sizeof(unsigned char) *            \
                 CHILD_STDIO_COUNT((buffer)) +      \
                 sizeof(HANDLE) * (fd)))


/* CRT file descriptor mode flags */
#define FOPEN       0x01
#define FEOFLAG     0x02
#define FCRLF       0x04
#define FPIPE       0x08
#define FNOINHERIT  0x10
#define FAPPEND     0x20
#define FDEV        0x40
#define FTEXT       0x80


/*
 * Clear the HANDLE_FLAG_INHERIT flag from all HANDLEs that were inherited
 * the parent process. Don't check for errors - the stdio handles may not be
 * valid, or may be closed already. There is no guarantee that this function
 * does a perfect job.
 */
void uv_disable_stdio_inheritance(void) {
  HANDLE handle;
  STARTUPINFOW si;

  /* Make the windows stdio handles non-inheritable. */
  handle = GetStdHandle(STD_INPUT_HANDLE);
  if (handle != NULL && handle != INVALID_HANDLE_VALUE)
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);

  handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle != NULL && handle != INVALID_HANDLE_VALUE)
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);

  handle = GetStdHandle(STD_ERROR_HANDLE);
  if (handle != NULL && handle != INVALID_HANDLE_VALUE)
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);

  /* Make inherited CRT FDs non-inheritable. */
  GetStartupInfoW(&si);
  if (uv__stdio_verify(si.lpReserved2, si.cbReserved2))
    uv__stdio_noinherit(si.lpReserved2);
}


static int uv__create_stdio_pipe_pair(uv_loop_t* loop,
    uv_pipe_t* server_pipe, HANDLE* child_pipe_ptr, unsigned int flags) {
  char pipe_name[64];
  SECURITY_ATTRIBUTES sa;
  DWORD server_access = 0;
  DWORD client_access = 0;
  HANDLE child_pipe = INVALID_HANDLE_VALUE;
  int err;

  if (flags & UV_PIPE_READABLE) {
    /* The server needs inbound access too, otherwise CreateNamedPipe() */
    /* won't give us the FILE_READ_ATTRIBUTES permission. We need that to */
    /* probe the state of the write buffer when we're trying to shutdown */
    /* the pipe. */
    server_access |= PIPE_ACCESS_OUTBOUND | PIPE_ACCESS_INBOUND;
    client_access |= GENERIC_READ | FILE_WRITE_ATTRIBUTES;
  }
  if (flags & UV_PIPE_WRITABLE) {
    server_access |= PIPE_ACCESS_INBOUND;
    client_access |= GENERIC_WRITE | FILE_READ_ATTRIBUTES;
  }

  /* Create server pipe handle. */
  err = uv_stdio_pipe_server(loop,
                             server_pipe,
                             server_access,
                             pipe_name,
                             sizeof(pipe_name));
  if (err)
    goto error;

  /* Create child pipe handle. */
  sa.nLength = sizeof sa;
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;

  child_pipe = CreateFileA(pipe_name,
                           client_access,
                           0,
                           &sa,
                           OPEN_EXISTING,
                           (flags & UV_PIPE_IPC) ? FILE_FLAG_OVERLAPPED : 0,
                           NULL);
  if (child_pipe == INVALID_HANDLE_VALUE) {
    err = GetLastError();
    goto error;
  }

#ifndef NDEBUG
  /* Validate that the pipe was opened in the right mode. */
  {
    DWORD mode;
    BOOL r = GetNamedPipeHandleState(child_pipe,
                                     &mode,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     0);
    assert(r == TRUE);
    assert(mode == (PIPE_READMODE_BYTE | PIPE_WAIT));
  }
#endif

  /* Do a blocking ConnectNamedPipe.  This should not block because we have */
  /* both ends of the pipe created. */
  if (!ConnectNamedPipe(server_pipe->handle, NULL)) {
    if (GetLastError() != ERROR_PIPE_CONNECTED) {
      err = GetLastError();
      goto error;
    }
  }

  /* The server end is now readable and/or writable. */
  if (flags & UV_PIPE_READABLE)
    server_pipe->flags |= UV_HANDLE_WRITABLE;
  if (flags & UV_PIPE_WRITABLE)
    server_pipe->flags |= UV_HANDLE_READABLE;

  *child_pipe_ptr = child_pipe;
  return 0;

 error:
  if (server_pipe->handle != INVALID_HANDLE_VALUE) {
    uv_pipe_cleanup(loop, server_pipe);
  }

  if (child_pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(child_pipe);
  }

  return err;
}


static int uv__duplicate_handle(uv_loop_t* loop, HANDLE handle, HANDLE* dup) {
  HANDLE current_process;


  /* _get_osfhandle will sometimes return -2 in case of an error. This seems */
  /* to happen when fd <= 2 and the process' corresponding stdio handle is */
  /* set to NULL. Unfortunately DuplicateHandle will happily duplicate */
  /* (HANDLE) -2, so this situation goes unnoticed until someone tries to */
  /* use the duplicate. Therefore we filter out known-invalid handles here. */
  if (handle == INVALID_HANDLE_VALUE ||
      handle == NULL ||
      handle == (HANDLE) -2) {
    *dup = INVALID_HANDLE_VALUE;
    return ERROR_INVALID_HANDLE;
  }

  current_process = GetCurrentProcess();

  if (!DuplicateHandle(current_process,
                       handle,
                       current_process,
                       dup,
                       0,
                       TRUE,
                       DUPLICATE_SAME_ACCESS)) {
    *dup = INVALID_HANDLE_VALUE;
    return GetLastError();
  }

  return 0;
}


static int uv__duplicate_fd(uv_loop_t* loop, int fd, HANDLE* dup) {
  HANDLE handle;

  if (fd == -1) {
    *dup = INVALID_HANDLE_VALUE;
    return ERROR_INVALID_HANDLE;
  }

  handle = uv__get_osfhandle(fd);
  return uv__duplicate_handle(loop, handle, dup);
}


int uv__create_nul_handle(HANDLE* handle_ptr,
    DWORD access) {
  HANDLE handle;
  SECURITY_ATTRIBUTES sa;

  sa.nLength = sizeof sa;
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;

  handle = CreateFileW(L"NUL",
                       access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa,
                       OPEN_EXISTING,
                       0,
                       NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    return GetLastError();
  }

  *handle_ptr = handle;
  return 0;
}


int uv__stdio_create(uv_loop_t* loop,
                     const uv_process_options_t* options,
                     BYTE** buffer_ptr) {
  BYTE* buffer;
  int count, i;
  int err;

  count = options->stdio_count;

  if (count < 0 || count > 255) {
    /* Only support FDs 0-255 */
    return ERROR_NOT_SUPPORTED;
  } else if (count < 3) {
    /* There should always be at least 3 stdio handles. */
    count = 3;
  }

  /* Allocate the child stdio buffer */
  buffer = (BYTE*) uv__malloc(CHILD_STDIO_SIZE(count));
  if (buffer == NULL) {
    return ERROR_OUTOFMEMORY;
  }

  /* Prepopulate the buffer with INVALID_HANDLE_VALUE handles so we can */
  /* clean up on failure. */
  CHILD_STDIO_COUNT(buffer) = count;
  for (i = 0; i < count; i++) {
    CHILD_STDIO_CRT_FLAGS(buffer, i) = 0;
    CHILD_STDIO_HANDLE(buffer, i) = INVALID_HANDLE_VALUE;
  }

  for (i = 0; i < count; i++) {
    uv_stream_t *stream;
    HANDLE stream_handle, child_handle;
    int crt_flags;

    if (i >= options->stdio_count) {
        stream = NULL;
    } else {
      if (options->stdio[i].type == UV_STREAM) {
          stream = options->stdio[i].data.stream;
      } else {
        /* Inherit a raw FD. */
        HANDLE child_handle;
        switch (options->stdio[i].type) {
          case UV_RAW_FD:
        /* Make an inheritable duplicate of the handle. */
            err = uv__duplicate_fd(loop, options->stdio[i].data.fd, &child_handle);
	        if (err != 0)
              goto error;
            break;
          case UV_RAW_HANDLE:
            err = uv__duplicate_handle(loop, options->stdio[i].data.os_handle, &child_handle);
            if (err != 0)
              goto error;
            break;
          default:
          goto error;
        }

        /* Figure out what the type is. */
        switch (GetFileType(child_handle)) {
          case FILE_TYPE_DISK:
            CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN;
            break;

          case FILE_TYPE_PIPE:
            CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FPIPE;

          case FILE_TYPE_CHAR:
          case FILE_TYPE_REMOTE:
            CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FDEV;
            break;

          case FILE_TYPE_UNKNOWN:
            if (GetLastError() != 0) {
              err = GetLastError();
              CloseHandle(child_handle);
              goto error;
            }
            CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FDEV;
            break;

          default:
            assert(0);
            return -1;
        }

        CHILD_STDIO_HANDLE(buffer, i) = child_handle;
        continue;
      }
    }

    if(stream == NULL) {
      /* Starting a process with no stdin/stout/stderr can confuse it. */
      /* So no matter what the user specified, we make sure the first */
      /* three FDs are always open in their typical modes, e.g. stdin */
      /* be readable and stdout/err should be writable. For FDs > 2, don't */
      /* do anything - all handles in the stdio buffer are initialized with */
      /* INVALID_HANDLE_VALUE, which should be okay. */
      if (i <= 2) {
        DWORD access = (i == 0) ? FILE_GENERIC_READ :
            (FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES);
        err = uv__create_nul_handle(&CHILD_STDIO_HANDLE(buffer, i), access);
        if (err != 0)
          goto error;
        CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FDEV;
      }
      continue;
    }

    /* Leech the handle out of the stream. */
    switch (stream->type) {
      case UV_TTY:
          stream_handle = ((uv_tty_t*) stream)->handle;
          crt_flags = FOPEN | FDEV;
          break;
      case UV_NAMED_PIPE:
          stream_handle = ((uv_pipe_t*) stream)->handle;
          crt_flags = FOPEN | FPIPE;
          break;
      default:
          stream_handle = INVALID_HANDLE_VALUE;
          crt_flags = 0;
      }

      if (stream_handle == NULL ||
          stream_handle == INVALID_HANDLE_VALUE) {
              /* The handle is already closed, or not yet created, or the */
              /* stream type is not supported. */
          err = ERROR_NOT_SUPPORTED;
              goto error;
      }

      /* Make an inheritable copy of the handle. */
      err = uv__duplicate_handle(loop, stream_handle, &child_handle);
      if (err != 0)
        goto error;

      CHILD_STDIO_HANDLE(buffer, i) = child_handle;
      CHILD_STDIO_CRT_FLAGS(buffer, i) = crt_flags;
  }

  *buffer_ptr  = buffer;
  return 0;

 error:
  uv__stdio_destroy(buffer);
  return err;
}


void uv__stdio_destroy(BYTE* buffer) {
  int i, count;

  count = CHILD_STDIO_COUNT(buffer);
  for (i = 0; i < count; i++) {
    HANDLE handle = CHILD_STDIO_HANDLE(buffer, i);
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }

  uv__free(buffer);
}


void uv__stdio_noinherit(BYTE* buffer) {
  int i, count;

  count = CHILD_STDIO_COUNT(buffer);
  for (i = 0; i < count; i++) {
    HANDLE handle = CHILD_STDIO_HANDLE(buffer, i);
    if (handle != INVALID_HANDLE_VALUE) {
      SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    }
  }
}


int uv__stdio_verify(BYTE* buffer, WORD size) {
  unsigned int count;

  /* Check the buffer pointer. */
  if (buffer == NULL)
    return 0;

  /* Verify that the buffer is at least big enough to hold the count. */
  if (size < CHILD_STDIO_SIZE(0))
    return 0;

  /* Verify if the count is within range. */
  count = CHILD_STDIO_COUNT(buffer);
  if (count > 256)
    return 0;

  /* Verify that the buffer size is big enough to hold info for N FDs. */
  if (size < CHILD_STDIO_SIZE(count))
    return 0;

  return 1;
}


WORD uv__stdio_size(BYTE* buffer) {
  return (WORD) CHILD_STDIO_SIZE(CHILD_STDIO_COUNT((buffer)));
}


HANDLE uv__stdio_handle(BYTE* buffer, int fd) {
  return CHILD_STDIO_HANDLE(buffer, fd);
}
