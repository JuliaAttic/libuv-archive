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
#include <signal.h>

#include "uv.h"
#include "internal.h"
#include "handle-inl.h"
#include "req-inl.h"


#define SIGKILL         9


/* CRT file descriptor mode flags */
#define FOPEN       0x01
#define FEOFLAG     0x02
#define FCRLF       0x04
#define FPIPE       0x08
#define FNOINHERIT  0x10
#define FAPPEND     0x20
#define FDEV        0x40
#define FTEXT       0x80


typedef struct env_var {
  const char* narrow;
  const wchar_t* wide;
  int len; /* including null or '=' */
  int supplied;
  int value_len;
} env_var_t;

#define E_V(str) { str "=", L##str, sizeof(str), 0, 0 }


#define UTF8_TO_UTF16(s, t)                               \
  size = uv_utf8_to_utf16(s, NULL, 0) * sizeof(wchar_t);  \
  t = (wchar_t*)malloc(size);                             \
  if (!t) {                                               \
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");          \
  }                                                       \
  if (!uv_utf8_to_utf16(s, t, size / sizeof(wchar_t))) {  \
    uv__set_sys_error(loop, GetLastError());              \
    err = -1;                                             \
    goto done;                                            \
  }


/* The `child_stdio_buffer` buffer has the following layout:
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

#define CHILD_STDIO_LPRESERVED2(buffer)             \
    ((LPBYTE) (buffer))

#define CHILD_STDIO_CBRESERVED2(buffer)             \
    CHILD_STDIO_SIZE(CHILD_STDIO_COUNT((buffer)))

#define CHILD_STDIO_CRT_FLAGS(buffer, fd)           \
    *((unsigned char*) (buffer) + sizeof(int) + fd)

#define CHILD_STDIO_HANDLE(buffer, fd)              \
    *((HANDLE*) ((unsigned char*) (buffer) +        \
                 sizeof(int) +                      \
                 sizeof(unsigned char) *            \
                 CHILD_STDIO_COUNT((buffer)) +      \
                 sizeof(HANDLE) * (fd)))


static void uv_process_init(uv_loop_t* loop, uv_process_t* handle) {
  uv_handle_init(loop, (uv_handle_t*) handle);
  handle->type = UV_PROCESS;
  handle->exit_cb = NULL;
  handle->pid = 0;
  handle->exit_signal = 0;
  handle->wait_handle = INVALID_HANDLE_VALUE;
  handle->process_handle = INVALID_HANDLE_VALUE;
  handle->close_handle = INVALID_HANDLE_VALUE;
  handle->child_stdio_buffer = NULL;

  uv_req_init(loop, (uv_req_t*)&handle->exit_req);
  handle->exit_req.type = UV_PROCESS_EXIT;
  handle->exit_req.data = handle;
  uv_req_init(loop, (uv_req_t*)&handle->close_req);
  handle->close_req.type = UV_PROCESS_CLOSE;
  handle->close_req.data = handle;

  loop->counters.handle_init++;
  loop->counters.process_init++;
}


/*
 * Path search functions
 */

/*
 * Helper function for search_path
 */
static wchar_t* search_path_join_test(const wchar_t* dir,
                                      int dir_len,
                                      const wchar_t* name,
                                      int name_len,
                                      const wchar_t* ext,
                                      int ext_len,
                                      const wchar_t* cwd,
                                      int cwd_len) {
  wchar_t *result, *result_pos;
  DWORD attrs;

  if (dir_len >= 1 && (dir[0] == L'/' || dir[0] == L'\\')) {
    /* It's a full path without drive letter, use cwd's drive letter only */
    cwd_len = 2;
  } else if (dir_len >= 2 && dir[1] == L':' &&
      (dir_len < 3 || (dir[2] != L'/' && dir[2] != L'\\'))) {
    /* It's a relative path with drive letter (ext.g. D:../some/file)
     * Replace drive letter in dir by full cwd if it points to the same drive,
     * otherwise use the dir only.
     */
    if (cwd_len < 2 || _wcsnicmp(cwd, dir, 2) != 0) {
      cwd_len = 0;
    } else {
      dir += 2;
      dir_len -= 2;
    }
  } else if (dir_len > 2 && dir[1] == L':') {
    /* It's an absolute path with drive letter
     * Don't use the cwd at all
     */
    cwd_len = 0;
  }

  /* Allocate buffer for output */
  result = result_pos = (wchar_t*)malloc(sizeof(wchar_t) *
      (cwd_len + 1 + dir_len + 1 + name_len + 1 + ext_len + 1));

  /* Copy cwd */
  wcsncpy(result_pos, cwd, cwd_len);
  result_pos += cwd_len;

  /* Add a path separator if cwd didn't end with one */
  if (cwd_len && wcsrchr(L"\\/:", result_pos[-1]) == NULL) {
    result_pos[0] = L'\\';
    result_pos++;
  }

  /* Copy dir */
  wcsncpy(result_pos, dir, dir_len);
  result_pos += dir_len;

  /* Add a separator if the dir didn't end with one */
  if (dir_len && wcsrchr(L"\\/:", result_pos[-1]) == NULL) {
    result_pos[0] = L'\\';
    result_pos++;
  }

  /* Copy filename */
  wcsncpy(result_pos, name, name_len);
  result_pos += name_len;

  if (ext_len) {
    /* Add a dot if the filename didn't end with one */
    if (name_len && result_pos[-1] != '.') {
      result_pos[0] = L'.';
      result_pos++;
    }

    /* Copy extension */
    wcsncpy(result_pos, ext, ext_len);
    result_pos += ext_len;
  }

  /* Null terminator */
  result_pos[0] = L'\0';

  attrs = GetFileAttributesW(result);

  if (attrs != INVALID_FILE_ATTRIBUTES &&
     !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    return result;
  }

  free(result);
  return NULL;
}


/*
 * Helper function for search_path
 */
static wchar_t* path_search_walk_ext(const wchar_t *dir,
                                     int dir_len,
                                     const wchar_t *name,
                                     int name_len,
                                     wchar_t *cwd,
                                     int cwd_len,
                                     int name_has_ext) {
  wchar_t* result;

  /* If the name itself has a nonempty extension, try this extension first */
  if (name_has_ext) {
    result = search_path_join_test(dir, dir_len,
                                   name, name_len,
                                   L"", 0,
                                   cwd, cwd_len);
    if (result != NULL) {
      return result;
    }
  }

  /* Try .com extension */
  result = search_path_join_test(dir, dir_len,
                                 name, name_len,
                                 L"com", 3,
                                 cwd, cwd_len);
  if (result != NULL) {
    return result;
  }

  /* Try .exe extension */
  result = search_path_join_test(dir, dir_len,
                                 name, name_len,
                                 L"exe", 3,
                                 cwd, cwd_len);
  if (result != NULL) {
    return result;
  }

  return NULL;
}


/*
 * search_path searches the system path for an executable filename -
 * the windows API doesn't provide this as a standalone function nor as an
 * option to CreateProcess.
 *
 * It tries to return an absolute filename.
 *
 * Furthermore, it tries to follow the semantics that cmd.exe, with this
 * exception that PATHEXT environment variable isn't used. Since CreateProcess
 * can start only .com and .exe files, only those extensions are tried. This
 * behavior equals that of msvcrt's spawn functions.
 *
 * - Do not search the path if the filename already contains a path (either
 *   relative or absolute).
 *
 * - If there's really only a filename, check the current directory for file,
 *   then search all path directories.
 *
 * - If filename specified has *any* extension, search for the file with the
 *   specified extension first.
 *
 * - If the literal filename is not found in a directory, try *appending*
 *   (not replacing) .com first and then .exe.
 *
 * - The path variable may contain relative paths; relative paths are relative
 *   to the cwd.
 *
 * - Directories in path may or may not end with a trailing backslash.
 *
 * - CMD does not trim leading/trailing whitespace from path/pathex entries
 *   nor from the environment variables as a whole.
 *
 * - When cmd.exe cannot read a directory, it will just skip it and go on
 *   searching. However, unlike posix-y systems, it will happily try to run a
 *   file that is not readable/executable; if the spawn fails it will not
 *   continue searching.
 *
 * TODO: correctly interpret UNC paths
 */
static wchar_t* search_path(const wchar_t *file,
                            wchar_t *cwd,
                            const wchar_t *path) {
  int file_has_dir;
  wchar_t* result = NULL;
  wchar_t *file_name_start;
  wchar_t *dot;
  const wchar_t *dir_start, *dir_end, *dir_path;
  int dir_len;
  int name_has_ext;

  int file_len = wcslen(file);
  int cwd_len = wcslen(cwd);

  /* If the caller supplies an empty filename,
   * we're not gonna return c:\windows\.exe -- GFY!
   */
  if (file_len == 0
      || (file_len == 1 && file[0] == L'.')) {
    return NULL;
  }

  /* Find the start of the filename so we can split the directory from the */
  /* name. */
  for (file_name_start = (wchar_t*)file + file_len;
       file_name_start > file
           && file_name_start[-1] != L'\\'
           && file_name_start[-1] != L'/'
           && file_name_start[-1] != L':';
       file_name_start--);

  file_has_dir = file_name_start != file;

  /* Check if the filename includes an extension */
  dot = wcschr(file_name_start, L'.');
  name_has_ext = (dot != NULL && dot[1] != L'\0');

  if (file_has_dir) {
    /* The file has a path inside, don't use path */
    result = path_search_walk_ext(
        file, file_name_start - file,
        file_name_start, file_len - (file_name_start - file),
        cwd, cwd_len,
        name_has_ext);

  } else {
    dir_end = path;

    /* The file is really only a name; look in cwd first, then scan path */
    result = path_search_walk_ext(L"", 0,
                                  file, file_len,
                                  cwd, cwd_len,
                                  name_has_ext);

    while (result == NULL) {
      if (*dir_end == L'\0') {
        break;
      }

      /* Skip the separator that dir_end now points to */
      if (dir_end != path) {
        dir_end++;
      }

      /* Next slice starts just after where the previous one ended */
      dir_start = dir_end;

      /* Slice until the next ; or \0 is found */
      dir_end = wcschr(dir_start, L';');
      if (dir_end == NULL) {
        dir_end = wcschr(dir_start, L'\0');
      }

      /* If the slice is zero-length, don't bother */
      if (dir_end - dir_start == 0) {
        continue;
      }

      dir_path = dir_start;
      dir_len = dir_end - dir_start;

      /* Adjust if the path is quoted. */
      if (dir_path[0] == '"' || dir_path[0] == '\'') {
        ++dir_path;
        --dir_len;
      }

      if (dir_path[dir_len - 1] == '"' || dir_path[dir_len - 1] == '\'') {
        --dir_len;
      }

      result = path_search_walk_ext(dir_path, dir_len,
                                    file, file_len,
                                    cwd, cwd_len,
                                    name_has_ext);
    }
  }

  return result;
}


/*
 * Quotes command line arguments
 * Returns a pointer to the end (next char to be written) of the buffer
 */
wchar_t* quote_cmd_arg(const wchar_t *source, wchar_t *target) {
  int len = wcslen(source),
      i, quote_hit;
  wchar_t* start;

  /*
   * Check if the string must be quoted;
   * if unnecessary, don't do it, it may only confuse older programs.
   */
  if (len == 0) {
    return target;
  }

  if (NULL == wcspbrk(source, L" \t\"")) {
    /* No quotation needed */
    wcsncpy(target, source, len);
    target += len;
    return target;
  }

  if (NULL == wcspbrk(source, L"\"\\")) {
    /*
     * No embedded double quotes or backlashes, so I can just wrap
     * quote marks around the whole thing.
     */
    *(target++) = L'"';
    wcsncpy(target, source, len);
    target += len;
    *(target++) = L'"';
    return target;
  }

  /*
   * Expected input/output:
   *   input : hello"world
   *   output: "hello\"world"
   *   input : hello""world
   *   output: "hello\"\"world"
   *   input : hello\world
   *   output: hello\world
   *   input : hello\\world
   *   output: hello\\world
   *   input : hello\"world
   *   output: "hello\\\"world"
   *   input : hello\\"world
   *   output: "hello\\\\\"world"
   *   input : hello world\
   *   output: "hello world\"
   */

  *(target++) = L'"';
  start = target;
  quote_hit = 1;

  for (i = len; i > 0; --i) {
    *(target++) = source[i - 1];

    if (quote_hit && source[i - 1] == L'\\') {
      *(target++) = L'\\';
    } else if(source[i - 1] == L'"') {
      quote_hit = 1;
      *(target++) = L'\\';
    } else {
      quote_hit = 0;
    }
  }
  target[0] = L'\0';
  wcsrev(start);
  *(target++) = L'"';
  return target;
}


wchar_t* make_program_args(char** args, int verbatim_arguments) {
  wchar_t* dst;
  wchar_t* ptr;
  char** arg;
  size_t size = 0;
  size_t len;
  int arg_count = 0;
  wchar_t* buffer;
  int arg_size;
  int buffer_size = 0;

  /* Count the required size. */
  for (arg = args; *arg; arg++) {
    arg_size = uv_utf8_to_utf16(*arg, NULL, 0) * sizeof(wchar_t);
    size += arg_size;
    buffer_size = arg_size > buffer_size ? arg_size : buffer_size;
    arg_count++;
  }

  /* Adjust for potential quotes. Also assume the worst-case scenario
  /* that every character needs escaping, so we need twice as much space. */
  size = size * 2 + arg_count * 2;

  dst = (wchar_t*)malloc(size);
  if (!dst) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  buffer = (wchar_t*)malloc(buffer_size);
  if (!buffer) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  ptr = dst;
  for (arg = args; *arg; arg++) {
    len = uv_utf8_to_utf16(*arg, buffer, (size_t)(size - (ptr - dst)));
    if (!len) {
      goto error;
    }
    if (verbatim_arguments) {
      wcscpy(ptr, buffer);
      ptr += len - 1;
    } else {
      ptr = quote_cmd_arg(buffer, ptr);
    }
    *ptr++ = *(arg + 1) ? L' ' : L'\0';
  }

  free(buffer);
  return dst;

error:
  free(dst);
  free(buffer);
  return NULL;
}


/*
 * If we learn that people are passing in huge environment blocks
 * then we should probably qsort() the array and then bsearch()
 * to see if it contains this variable. But there are ownership
 * issues associated with that solution; this is the caller's
 * char**, and modifying it is rude.
 */
static void check_required_vars_contains_var(env_var_t* required, int size,
    const char* var) {
  int i;
  for (i = 0; i < size; ++i) {
    if (_strnicmp(required[i].narrow, var, required[i].len) == 0) {
      required[i].supplied =  1;
      return;
    }
  }
}


/*
 * The way windows takes environment variables is different than what C does;
 * Windows wants a contiguous block of null-terminated strings, terminated
 * with an additional null.
 *
 * Windows has a few "essential" environment variables. winsock will fail
 * to initialize if SYSTEMROOT is not defined; some APIs make reference to
 * TEMP. SYSTEMDRIVE is probably also important. We therefore ensure that
 * these get defined if the input environment block does not contain any
 * values for them.
 */
wchar_t* make_program_env(char** env_block) {
  wchar_t* dst;
  wchar_t* ptr;
  char** env;
  int env_len = 1 * sizeof(wchar_t); /* room for closing null */
  int len;
  int i;
  DWORD var_size;

  env_var_t required_vars[] = {
    E_V("SYSTEMROOT"),
    E_V("SYSTEMDRIVE"),
    E_V("TEMP"),
  };

  for (env = env_block; *env; env++) {
    check_required_vars_contains_var(required_vars,
                                     ARRAY_SIZE(required_vars),
                                     *env);
    env_len += (uv_utf8_to_utf16(*env, NULL, 0) * sizeof(wchar_t));
  }

  for (i = 0; i < ARRAY_SIZE(required_vars); ++i) {
    if (!required_vars[i].supplied) {
      env_len += required_vars[i].len * sizeof(wchar_t);
      var_size = GetEnvironmentVariableW(required_vars[i].wide, NULL, 0);
      if (var_size == 0) {
        uv_fatal_error(GetLastError(), "GetEnvironmentVariableW");
      }
      required_vars[i].value_len = (int)var_size;
      env_len += (int)var_size * sizeof(wchar_t);
    }
  }

  dst = malloc(env_len);
  if (!dst) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  ptr = dst;

  for (env = env_block; *env; env++, ptr += len) {
    len = uv_utf8_to_utf16(*env, ptr, (size_t)(env_len - (ptr - dst)));
    if (!len) {
      free(dst);
      return NULL;
    }
  }

  for (i = 0; i < ARRAY_SIZE(required_vars); ++i) {
    if (!required_vars[i].supplied) {
      wcscpy(ptr, required_vars[i].wide);
      ptr += required_vars[i].len - 1;
      *ptr++ = L'=';
      var_size = GetEnvironmentVariableW(required_vars[i].wide,
                                         ptr,
                                         required_vars[i].value_len);
      if (var_size == 0) {
        uv_fatal_error(GetLastError(), "GetEnvironmentVariableW");
      }
      ptr += required_vars[i].value_len;
    }
  }

  *ptr = L'\0';
  return dst;
}


static int uv_create_stdio_pipe_pair(uv_loop_t* loop, uv_pipe_t* server_pipe,
    HANDLE* child_pipe_ptr, unsigned int flags) {
  char pipe_name[64];
  SECURITY_ATTRIBUTES sa;
  DWORD server_access = 0;
  DWORD client_access = 0;
  HANDLE child_pipe = INVALID_HANDLE_VALUE;

  if (flags & UV_READABLE_PIPE) {
    server_access |= PIPE_ACCESS_OUTBOUND;
    client_access |= GENERIC_READ | FILE_WRITE_ATTRIBUTES;
  }
  if (flags & UV_WRITABLE_PIPE) {
    server_access |= PIPE_ACCESS_INBOUND;
    client_access |= GENERIC_WRITE;
  }

  /* Create server pipe handle. */
  if (uv_stdio_pipe_server(loop,
                           server_pipe,
                           server_access,
                           pipe_name,
                           sizeof(pipe_name)) < 0) {
    goto error;
  }

  /* Create child pipe handle. */
  sa.nLength = sizeof sa;
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;

  child_pipe = CreateFileA(pipe_name,
                           client_access,
                           0,
                           &sa,
                           OPEN_EXISTING,
                           server_pipe->ipc ? FILE_FLAG_OVERLAPPED : 0,
                           NULL);
  if (child_pipe == INVALID_HANDLE_VALUE) {
    uv__set_sys_error(loop, GetLastError());
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
      uv__set_sys_error(loop, GetLastError());
      goto error;
    }
  }

  *child_pipe_ptr = child_pipe;
  return 0;

 error:
  if (server_pipe->handle != INVALID_HANDLE_VALUE) {
    uv_pipe_cleanup(loop, server_pipe);
  }

  if (child_pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(child_pipe);
  }

  return -1;
}


static int duplicate_handle(uv_loop_t* loop, HANDLE handle, HANDLE* dup) {
  HANDLE current_process;

  current_process = GetCurrentProcess();

  if (!DuplicateHandle(current_process,
                       handle,
                       current_process,
                       dup,
                       0,
                       TRUE,
                       DUPLICATE_SAME_ACCESS)) {
    *dup = INVALID_HANDLE_VALUE;
    uv__set_sys_error(loop, GetLastError());
    return -1;
  }

  return 0;
}


static int duplicate_fd(uv_loop_t* loop, int fd, HANDLE* dup) {
  HANDLE handle;

  if (fd == -1) {
    *dup = INVALID_HANDLE_VALUE;
    uv__set_artificial_error(loop, UV_EBADF);
    return -1;
  }

  handle = (HANDLE)_get_osfhandle(fd);
  return duplicate_handle(loop, handle, dup);
}


static int create_nul_handle(uv_loop_t* loop, HANDLE* handle_ptr,
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
    uv__set_sys_error(loop, GetLastError());
    return -1;
  }

  *handle_ptr = handle;
  return 0;
}


static void set_child_stdio_noinherit(void* buffer) {
  int i, count;

  count = CHILD_STDIO_COUNT(buffer);
  for (i = 0; i < count; i++) {
    HANDLE handle = CHILD_STDIO_HANDLE(buffer, i);
    if (handle != INVALID_HANDLE_VALUE) {
      SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    }
  }
}


static void close_and_free_child_stdio(void* buffer) {
  int i, count;

  count = CHILD_STDIO_COUNT(buffer);
  for (i = 0; i < count; i++) {
    HANDLE handle = CHILD_STDIO_HANDLE(buffer, i);
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }

  free(buffer);
}


/*
 * Called on Windows thread-pool thread to indicate that
 * a child process has exited.
 */
static void CALLBACK exit_wait_callback(void* data, BOOLEAN didTimeout) {
  uv_process_t* process = (uv_process_t*)data;
  uv_loop_t* loop = process->loop;

  assert(didTimeout == FALSE);
  assert(process);

  /* Post completed */
  POST_COMPLETION_FOR_REQ(loop, &process->exit_req);
}


/*
 * Called on Windows thread-pool thread to indicate that
 * UnregisterWaitEx has completed.
 */
static void CALLBACK close_wait_callback(void* data, BOOLEAN didTimeout) {
  uv_process_t* process = (uv_process_t*)data;
  uv_loop_t* loop = process->loop;

  assert(didTimeout == FALSE);
  assert(process);

  /* Post completed */
  POST_COMPLETION_FOR_REQ(loop, &process->close_req);
}


/*
 * Called on windows thread pool when CreateProcess failed. It writes an error
 * message to the process' intended stderr and then posts a PROCESS_EXIT
 * packet to the completion port.
 */
static DWORD WINAPI spawn_failure(void* data) {
  char syscall[] = "CreateProcessW: ";
  char unknown[] = "unknown error\n";
  uv_process_t* process = (uv_process_t*) data;
  uv_loop_t* loop = process->loop;
  HANDLE child_stderr = CHILD_STDIO_HANDLE(process->child_stdio_buffer, 2);
  char* buf = NULL;
  DWORD count, written;

  if (child_stderr != INVALID_HANDLE_VALUE) {
    WriteFile(child_stderr, syscall, sizeof(syscall) - 1, &written, NULL);

    count = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL,
                           process->spawn_errno,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           (LPSTR) &buf,
                           0,
                           NULL);

    if (buf != NULL && count > 0) {
      WriteFile(child_stderr, buf, count, &written, NULL);
      LocalFree(buf);
    } else {
      WriteFile(child_stderr, unknown, sizeof(unknown) - 1, &written, NULL);
    }

    FlushFileBuffers(child_stderr);
  }

  /* Post completed */
  POST_COMPLETION_FOR_REQ(loop, &process->exit_req);

  return 0;
}


/* Called on main thread after a child process has exited. */
void uv_process_proc_exit(uv_loop_t* loop, uv_process_t* handle) {
  DWORD exit_code;

  /* FIXME: race condition. */
  if (handle->flags & UV_HANDLE_CLOSING) {
    return;
  }

  /* Unregister from process notification. */
  if (handle->wait_handle != INVALID_HANDLE_VALUE) {
    UnregisterWait(handle->wait_handle);
    handle->wait_handle = INVALID_HANDLE_VALUE;
  }

  if (handle->process_handle == INVALID_HANDLE_VALUE ||
      !GetExitCodeProcess(handle->process_handle, &exit_code)) {
    /* The process never even started in the first place, or we were unable */
    /* to obtain the exit code. */
    exit_code = 127;
  }

  /* Set the handle to inactive: no callbacks will be made after the exit */
  /* callback.*/
  uv__handle_stop(handle);

  /* Fire the exit callback. */
  if (handle->exit_cb) {
    handle->exit_cb(handle, exit_code, handle->exit_signal);
  }
}


/* Called on main thread after UnregisterWaitEx finishes. */
void uv_process_proc_close(uv_loop_t* loop, uv_process_t* handle) {
  uv_want_endgame(loop, (uv_handle_t*)handle);
}


void uv_process_close(uv_loop_t* loop, uv_process_t* handle) {
  uv__handle_start(handle);

  if (handle->wait_handle != INVALID_HANDLE_VALUE) {
    handle->close_handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    UnregisterWaitEx(handle->wait_handle, handle->close_handle);
    handle->wait_handle = NULL;

    RegisterWaitForSingleObject(&handle->wait_handle, handle->close_handle,
        close_wait_callback, (void*)handle, INFINITE,
        WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
  } else {
    uv_want_endgame(loop, (uv_handle_t*)handle);
  }
}


void uv_process_endgame(uv_loop_t* loop, uv_process_t* handle) {
  if (handle->flags & UV_HANDLE_CLOSING) {
    assert(!(handle->flags & UV_HANDLE_CLOSED));
    uv__handle_stop(handle);

    /* Clean-up the process handle. */
    CloseHandle(handle->process_handle);

    /* Clean up the child stdio ends that may have been left open. */
    if (handle->child_stdio_buffer != NULL) {
      close_and_free_child_stdio(handle->child_stdio_buffer);
    }

    uv__handle_close(handle);
  }
}


static int init_child_stdio(uv_loop_t* loop, uv_process_options_t* options,
    void** buffer_ptr) {
  void* buffer;
  int count, i;

  count = options->stdio_count;

  if (count < 0 || count > 255) {
    /* Only support FDs 0-255 */
    uv__set_artificial_error(loop, UV_ENOTSUP);
    return -1;
  } else if (count < 3) {
    /* There should always be at least 3 stdio handles. */
    count = 3;
  }

  /* Allocate the child stdio buffer */
  buffer = malloc(CHILD_STDIO_SIZE(count));
  if (buffer == NULL) {
    uv__set_artificial_error(loop, UV_ENOMEM);
    return -1;
  }

  /* Prepopulate the buffer with INVALID_HANDLE_VALUE handles so we can */
  /* clean up on failure. */
  CHILD_STDIO_COUNT(buffer) = count;
  for (i = 0; i < count; i++) {
    CHILD_STDIO_CRT_FLAGS(buffer, i) = 0;
    CHILD_STDIO_HANDLE(buffer, i) = INVALID_HANDLE_VALUE;
  }

  for (i = 0; i < count; i++) {
    uv_stdio_container_t fdopt;
    if (i < options->stdio_count) {
      fdopt = options->stdio[i];
    } else {
      fdopt.flags = UV_IGNORE;
    }

    switch (fdopt.flags & (UV_IGNORE | UV_CREATE_PIPE | UV_INHERIT_FD |
            UV_INHERIT_STREAM)) {
      case UV_IGNORE:
        /* Starting a process with no stdin/stout/stderr can confuse it. */
        /* So no matter what the user specified, we make sure the first */
        /* three FDs are always open in their typical modes, e.g. stdin */
        /* be readable and stdout/err should be writable. For FDs > 2, don't */
        /* do anything - all handles in the stdio buffer are initialized with */
        /* INVALID_HANDLE_VALUE, which should be okay. */
        if (i <= 2) {
          DWORD access = (i == 0) ? FILE_GENERIC_READ :
                                    FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES;
          if (create_nul_handle(loop,
                                &CHILD_STDIO_HANDLE(buffer, i),
                                access) < 0) {
            goto error;
          }
          CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FDEV;
        }
        break;

      case UV_CREATE_PIPE: {
        /* Create a pair of two connected pipe ends; one end is turned into */
        /* an uv_pipe_t for use by the parent. The other one is given to */
        /* the child. */
        uv_pipe_t* parent_pipe = (uv_pipe_t*) fdopt.data.stream;
        HANDLE child_pipe;

        /* Create a new, connected pipe pair. stdio[i].stream should point */
        /* to an uninitialized, but not connected pipe handle. */
        assert(fdopt.data.stream->type == UV_NAMED_PIPE);
        assert(!(fdopt.data.stream->flags & UV_HANDLE_CONNECTION));
        assert(!(fdopt.data.stream->flags & UV_HANDLE_PIPESERVER));

        if (uv_create_stdio_pipe_pair(loop,
                                      parent_pipe,
                                      &child_pipe,
                                      fdopt.flags) < 0) {
          goto error;
        }

        CHILD_STDIO_HANDLE(buffer, i) = child_pipe;
        CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FPIPE;
        break;
      }

      case UV_INHERIT_FD: {
        /* Inherit a raw FD. */
        HANDLE child_handle;

        /* Make an inheritable duplicate of the handle. */
        if (duplicate_fd(loop, fdopt.data.fd, &child_handle) < 0) {
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
            if (GetLastError != 0) {
              uv__set_sys_error(loop, GetLastError());
              CloseHandle(child_handle);
              goto error;
            }
            CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FDEV;
            break;

          default:
            assert(0);
        }

        CHILD_STDIO_HANDLE(buffer, i) = child_handle;
        break;
      }

      case UV_INHERIT_STREAM: {
        /* Use an existing stream as the stdio handle for the child. */
        HANDLE stream_handle, child_handle;
        unsigned char crt_flags;
        uv_stream_t* stream = fdopt.data.stream;

        /* Leech the handle out of the stream. */
        if (stream->type = UV_TTY) {
          stream_handle = ((uv_tty_t*) stream)->handle;
          crt_flags = FOPEN | FDEV;
        } else if (stream->type == UV_NAMED_PIPE &&
                   stream->flags & UV_HANDLE_CONNECTED) {
          stream_handle = ((uv_pipe_t*) stream)->handle;
          crt_flags = FOPEN | FPIPE;
        } else {
          stream_handle = INVALID_HANDLE_VALUE;
          crt_flags = 0;
        }

        if (stream_handle == NULL ||
            stream_handle == INVALID_HANDLE_VALUE) {
          /* The handle is already closed, or not yet created, or the */
          /* stream type is not supported. */
          uv__set_artificial_error(loop, UV_ENOTSUP);
          goto error;
        }

        /* Make an inheritable copy of the handle. */
        if (duplicate_handle(loop,
                             stream_handle,
                             &child_handle) < 0) {
          goto error;
        }

        CHILD_STDIO_HANDLE(buffer, i) = child_handle;
        CHILD_STDIO_CRT_FLAGS(buffer, i) = crt_flags;
        break;
      }

      default:
        assert(0);
    }
  }

  *buffer_ptr  = buffer;
  return 0;

 error:
  close_and_free_child_stdio(buffer);
  return -1;
}


int uv_spawn(uv_loop_t* loop, uv_process_t* process,
    uv_process_options_t options) {
  int size, err = 0, keep_child_stdio_open = 0;
  wchar_t* path = NULL;
  BOOL result;
  wchar_t* application_path = NULL, *application = NULL, *arguments = NULL,
           *env = NULL, *cwd = NULL;
  STARTUPINFOW startup;
  PROCESS_INFORMATION info;
  DWORD process_flags;

  if (options.flags & (UV_PROCESS_SETGID | UV_PROCESS_SETUID)) {
    uv__set_artificial_error(loop, UV_ENOTSUP);
    return -1;
  }

  assert(options.file != NULL);
  assert(!(options.flags & ~(UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS |
                             UV_PROCESS_DETACHED |
                             UV_PROCESS_SETGID |
                             UV_PROCESS_SETUID)));

  uv_process_init(loop, process);

  process->exit_cb = options.exit_cb;
  UTF8_TO_UTF16(options.file, application);
  arguments = options.args ? make_program_args(options.args,
      options.flags & UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS) : NULL;
  env = options.env ? make_program_env(options.env) : NULL;

  if (options.cwd) {
    UTF8_TO_UTF16(options.cwd, cwd);
  } else {
    size  = GetCurrentDirectoryW(0, NULL) * sizeof(wchar_t);
    if (size) {
      cwd = (wchar_t*)malloc(size);
      if (!cwd) {
        uv__set_artificial_error(loop, UV_ENOMEM);
        err = -1;
        goto done;
      }

      GetCurrentDirectoryW(size, cwd);
    } else {
      uv__set_sys_error(loop, GetLastError());
      err = -1;
      goto done;
    }
  }

  /* Get PATH env. variable. */
  size = GetEnvironmentVariableW(L"PATH", NULL, 0) + 1;
  path = (wchar_t*)malloc(size * sizeof(wchar_t));
  if (!path) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }
  GetEnvironmentVariableW(L"PATH", path, size * sizeof(wchar_t));
  path[size - 1] = L'\0';

  application_path = search_path(application,
                                 cwd,
                                 path);

  if (!application_path) {
    /* CreateProcess will fail, but this allows us to pass this error to */
    /* the user asynchronously. */
    application_path = application;
  }


  if (init_child_stdio(loop, &options, &process->child_stdio_buffer) < 0) {
     err = -1;
     goto done;
  }

  startup.cb = sizeof(startup);
  startup.lpReserved = NULL;
  startup.lpDesktop = NULL;
  startup.lpTitle = NULL;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.cbReserved2 = CHILD_STDIO_CBRESERVED2(process->child_stdio_buffer);
  startup.lpReserved2 = CHILD_STDIO_LPRESERVED2(process->child_stdio_buffer);
  startup.hStdInput = CHILD_STDIO_HANDLE(process->child_stdio_buffer, 0);
  startup.hStdOutput = CHILD_STDIO_HANDLE(process->child_stdio_buffer, 1);
  startup.hStdError = CHILD_STDIO_HANDLE(process->child_stdio_buffer, 2);

  process_flags = CREATE_UNICODE_ENVIRONMENT;
  if (options.flags & UV_PROCESS_DETACHED) {
    process_flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
  }

  if (CreateProcessW(application_path,
                     arguments,
                     NULL,
                     NULL,
                     1,
                     process_flags,
                     env,
                     cwd,
                     &startup,
                     &info)) {
    /* Spawn succeeded */
    process->process_handle = info.hProcess;
    process->pid = info.dwProcessId;

    if (options.stdio_count > 0 &&
        options.stdio[0].flags & UV_CREATE_PIPE &&
        options.stdio[0].data.stream->type == UV_NAMED_PIPE &&
        ((uv_pipe_t*)options.stdio[0].data.stream)->ipc) {
      ((uv_pipe_t*)options.stdio[0].data.stream)->ipc_pid = info.dwProcessId;
    }

    /* Setup notifications for when the child process exits. */
    result = RegisterWaitForSingleObject(&process->wait_handle,
        process->process_handle, exit_wait_callback, (void*)process, INFINITE,
        WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);
    if (!result) {
      uv_fatal_error(GetLastError(), "RegisterWaitForSingleObject");
    }

    CloseHandle(info.hThread);

  } else {
    /* CreateProcessW failed, but this failure should be delivered */
    /* asynchronously to retain unix compatibility. So pretend spawn */
    /* succeeded, and start a thread instead that prints an error */
    /* to the child's intended stderr. */
    process->spawn_errno = GetLastError();
    keep_child_stdio_open = 1;
    if (!QueueUserWorkItem(spawn_failure, process, WT_EXECUTEDEFAULT)) {
      uv_fatal_error(GetLastError(), "QueueUserWorkItem");
    }
  }

done:
  free(application);
  if (application_path != application) {
    free(application_path);
  }
  free(arguments);
  free(cwd);
  free(env);
  free(path);

  /* Under normal circumstances we should close the stdio handles now - the */
  /* the child now has its own duplicates, or something went horribly wrong */
  /* The only exception is when CreateProcess has failed, then we actually */
  /* need to keep the stdio handles to report the error asynchronously. */
  if (process->child_stdio_buffer == NULL) {
    /* Something went wrong before child stdio was initialized. */
  } else if (!keep_child_stdio_open) {
    close_and_free_child_stdio(process->child_stdio_buffer);
    process->child_stdio_buffer = NULL;
  } else {
    /* We're keeping the handles open, the thread pool is going to have */
    /* it's way with them. But at least make them non-inheritable. */
    set_child_stdio_noinherit(process->child_stdio_buffer);
  }

  if (err == 0) {
    /* Spawn was succesful. The handle will be active until the exit */
    /* is made or the handle is closed, whichever happens first. */
    uv__handle_start(process);
  } else {
    /* Spawn was not successful. Clean up. */
    if (process->wait_handle != INVALID_HANDLE_VALUE) {
      UnregisterWait(process->wait_handle);
      process->wait_handle = INVALID_HANDLE_VALUE;
    }

    if (process->process_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(process->process_handle);
      process->process_handle = INVALID_HANDLE_VALUE;
    }
  }

  return err;
}


static uv_err_t uv__kill(HANDLE process_handle, int signum) {
  DWORD status;
  uv_err_t err;

  if (signum == SIGTERM || signum == SIGKILL || signum == SIGINT) {
    /* Kill the process. On Windows, killed processes normally return 1. */
    if (TerminateProcess(process_handle, 1)) {
      err = uv_ok_;
    } else {
      err = uv__new_sys_error(GetLastError());
    }
  } else if (signum == 0) {
    /* Health check: is the process still alive? */
    if (GetExitCodeProcess(process_handle, &status)) {
      if (status == STILL_ACTIVE) {
        err =  uv_ok_;
      } else {
        err = uv__new_artificial_error(UV_ESRCH);
      }
    } else {
      err = uv__new_sys_error(GetLastError());
    }
  } else {
    err = uv__new_artificial_error(UV_ENOSYS);
  }

  return err;
}


int uv_process_kill(uv_process_t* process, int signum) {
  uv_err_t err;

  if (process->process_handle == INVALID_HANDLE_VALUE) {
    uv__set_artificial_error(process->loop, UV_EINVAL);
    return -1;
  }

  err = uv__kill(process->process_handle, signum);

  if (err.code != UV_OK) {
    uv__set_error(process->loop, err.code, err.sys_errno_);
    return -1;
  }

  process->exit_signal = signum;

  return 0;
}


uv_err_t uv_kill(int pid, int signum) {
  uv_err_t err;
  HANDLE process_handle = OpenProcess(PROCESS_TERMINATE |
    PROCESS_QUERY_INFORMATION, FALSE, pid);

  if (process_handle == NULL) {
    if (GetLastError() == ERROR_INVALID_PARAMETER) {
      return uv__new_artificial_error(UV_ESRCH);
    } else {
      return uv__new_sys_error(GetLastError());
    }
  }

  err = uv__kill(process_handle, signum);
  CloseHandle(process_handle);

  return err;
}
