SERVER = server
CLIENT = client

CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++11
LDFLAGS = 

SERVER_SRC = server.cpp
CLIENT_SRC = client.cpp

all: $(SERVER) $(CLIENT)

$(SERVER): $(SERVER_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(CLIENT): $(CLIENT_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(SERVER) $(CLIENT)

run-server: $(SERVER)
	./$(SERVER)

run-client: $(CLIENT)
	./$(CLIENT)

.PHONY: all clean run-server run-client
