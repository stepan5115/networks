CXX = g++
CXXFLAGS = -Wall -Wextra -pthread -O2 -Wno-stringop-truncation -Wno-unused-parameter -Wno-format-truncation
LDFLAGS = -pthread

TARGETS = server client
OBJS = 

all: $(TARGETS)

server: server.cpp structs.h
	$(CXX) $(CXXFLAGS) -o server server.cpp $(LDFLAGS)

client: client.cpp structs.h
	$(CXX) $(CXXFLAGS) -o client client.cpp $(LDFLAGS)

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean