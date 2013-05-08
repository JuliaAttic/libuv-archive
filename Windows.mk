!INCLUDE <Windows.inc>

.SUFFIXES: .c

NAME = uv

HEADERS = \
	src\uv-common.h	\
	src\win\atomicops-inl.h  \
	src\win\winapi.h  \
	src\win\internal.h \
	src\win\stream-inl.h \
	src\win\handle-inl.h \
	src\win\winsock.h \
	src\win\req-inl.h \
	include\uv.h \
	include\uv-private\uv-win.h \
	include\uv-private\tree.h \
	include\uv-private\ngx-queue.h

OBJECTS = \
	src\inet.obj \
	src\uv-common.obj \
	src\fs-poll.obj \
	src\win\loop-watcher.obj \
	src\win\process.obj \
	src\win\tty.obj \
	src\win\util.obj \
	src\win\dl.obj \
	src\win\stream.obj \
	src\win\getaddrinfo.obj \
	src\win\signal.obj \
	src\win\winapi.obj \
	src\win\thread.obj \
	src\win\fs.obj \
	src\win\pipe.obj \
	src\win\tcp.obj \
	src\win\fs-event.obj \
	src\win\core.obj \
	src\win\udp.obj \
	src\win\async.obj \
	src\win\process-stdio.obj \
	src\win\poll.obj \
	src\win\threadpool.obj \
	src\win\error.obj \
	src\win\req.obj \
	src\win\winsock.obj \
	src\win\handle.obj \
	src\win\timer.obj

INCLUDE = $(INCLUDE);$(MAKEDIR)\include;$(MAKEDIR)\include\uv-private
CFLAGS = $(CFLAGS) -D_CRT_SECURE_NO_WARNINGS

default: libuv

libuv: lib$(NAME).lib

lib$(NAME).lib: $(OBJECTS)
	$(AR) /OUT:$@ $**

{src}.c{src}.obj:
	$(CC) $(CFLAGS) /Fo$@ $<

{src\win}.c{src\win}.obj:
	$(CC) $(CFLAGS) /Fo$@ $< 

clean:
	del /Q $(OBJECTS)
	del /Q lib$(NAME).lib
	del /Q *.pdb

# vim: noexpandtab:ts=4:sw=4:

