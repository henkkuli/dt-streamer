CC=g++
CCFLAGS=-std=c++17 -Wall -Wextra -Wshadow -Werror -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
		-g $(CFLAGS_EXTRA)
LDFLAGS=-lavformat -lavcodec -lavutil -lavdevice -lswscale -lpthread -lboost_system -lprotobuf

all: dt-streamer

dt-streamer: main.cpp
	$(CC) $(CCFLAGS) $(LDFLAGS) -o $@ $^
