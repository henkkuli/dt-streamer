CC=g++
CCFLAGS=-std=c++17 -Wall -Wextra -Wshadow -Werror -Wno-unused-parameter -Wno-unused-variable \
		-Wno-unused-but-set-variable -g $(CFLAGS_EXTRA)
LDFLAGS=-lavformat -lavcodec -lavutil -lavdevice -lswscale -lpthread -lboost_system -lprotobuf \
		`pkg-config --libs protobuf grpc++`

PROTOC=protoc
PROTOFLAGS=-I./ --cpp_out=./ --grpc_out=./ --plugin=protoc-gen-grpc=`which grpc_cpp_plugin`


all: server client

dt-streamer: main.cpp messages.pb.cc
	$(CC) $(CCFLAGS) $(LDFLAGS) -o $@ $^

server: server.cpp control.pb.cc control.grpc.pb.cc messages.pb.cc Logger.cpp
	$(CC) $(CCFLAGS) $(LDFLAGS) -o $@ $^

client: client.cpp control.pb.cc control.grpc.pb.cc messages.pb.cc Logger.cpp
	$(CC) $(CCFLAGS) $(LDFLAGS) -o $@ $^

messages.pb.cc: messages.proto
	$(PROTOC) $(PROTOFLAGS) $^

control.pb.cc: control.proto
	$(PROTOC) $(PROTOFLAGS) $^

.PHONY: clean
clean:
	rm server client
