all: server client

# General compilation options
CXX = g++
CXXFLAGS = -std=c++17 -g
CXXFLAGS += -Wall -Wextra -Wshadow -Werror -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
CXXFLAGS += $(CXXFLAGS_EXTRA)

# Proto compilation options
PROTOC = protoc
PROTOFLAGS = -I./ --cpp_out=./ --grpc_out=./ --plugin=protoc-gen-grpc=`which grpc_cpp_plugin`

# Common linking options
LDLIBS = -lavformat -lavcodec -lavutil -lavdevice -lswscale -lpthread -lboost_system -lboost_coroutine -lprotobuf
		 
# Target specific linking options
SERVER_LIBS = -lgrpc++ -lboost_program_options
CLIENT_LIBS =

# List the compiled sources for each executable
SERVER_SRC = server.cpp Logger.cpp control.proto messages.proto
CLIENT_SRC = client.cpp Logger.cpp messages.proto

# Turn proto files into corresponding c++ files
SERVER_PROTO_C = $(filter %.pb.cc,$(SERVER_SRC:%.proto=%.pb.cc) $(SERVER_SRC:%.proto=%.grpc.pb.cc))
CLIENT_PROTO_C = $(filter %.pb.cc,$(CLIENT_SRC:%.proto=%.pb.cc) $(CLIENT_SRC:%.proto=%.grpc.pb.cc))

# Keep the proto files between compilations
.SECONDARY: $(SERVER_PROTO_C) $(CLIENT_PROTO_C)

# Make all c++ files depend on the proto files, especially the headers
$(filter %.cpp,$(SERVER_SRC)): $(SERVER_PROTO_C:%.cc=%.h)

# Generate list of object files
SERVER_OBJS = $(filter %.o,$(SERVER_SRC:%.cpp=%.o)) $(SERVER_PROTO_C:%.cc=%.o)
CLIENT_OBJS = $(filter %.o,$(CLIENT_SRC:%.cpp=%.o)) $(CLIENT_PROTO_C:%.cc=%.o)

# Include all dependencies
-include $(patsubst %.o,%.d,$(SERVER_OBJS) $(CLIENT_OBJS))

server: $(SERVER_OBJS)
	$(CXX) $(LDFLAGS) $(LDLIBS) $(SERVER_LIBS) -o $@ $^

client: $(CLIENT_OBJS)
	$(CXX) $(LDFLAGS) $(LDLIBS) $(CLIENT_LIBS) -o $@ $^

# Automatic compilation for c++ files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $<
	$(CXX) $(CXXFLAGS) -MM $< > $(@:.o=.d)

# Automatic compilation for proto files
%.pb.cc %.pb.h %.grpc.pb.cc %.grpc.pb.h: %.proto
	$(PROTOC) $(PROTOFLAGS) $^

%.o: %.cc
	$(CXX) $(CXXFLAGS) -o $@ -c $<
	$(CXX) $(CXXFLAGS) -MM $< > $(@:.o=.d)


.PHONY: clean
clean:
	rm -f server client $(SERVER_OBJS) $(CLIENT_OBJS) $(SERVER_PROTO_C) $(CLIENT_PROTO_C) \
	      $(SERVER_OBJS:%.o=%.d) $(CLIENT_OBJS:%.o=%.d) $(SERVER_PROTO_C:%.cc=%.h) $(CLIENT_PROTO_C:%.cc=%.h) 
