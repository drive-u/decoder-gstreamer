CC = g++
CFLAGS = -Wall -Wextra -pedantic -std=c++11 -std=c++2a -I../interface -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I../../cpp

LDFLAGS = -shared
LIBS = -lgstreamer-1.0 -lgstapp-1.0 -lgobject-2.0 -lglib-2.0 -lgstvideo-1.0 -lgstbase-1.0
SRCS = RunGstreamerDecoder.cpp
OBJS = $(SRCS:.cpp=.o)
LIBNAME = decoder_gstreamer.so

.PHONY: all clean

all: $(LIBNAME)

$(LIBNAME): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CC) $(CFLAGS) -fPIC -c -o $@ $< $(LIBS)

clean:
	rm -f $(OBJS) $(LIBNAME)
