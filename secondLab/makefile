SERVER = server
CLIENT = client

CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++11

SERVER_SRC = server.cpp
CLIENT_SRC = client.cpp
HEADERS = structs.h

all: $(SERVER) $(CLIENT)

$(SERVER): $(SERVER_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SERVER_SRC)

$(CLIENT): $(CLIENT_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(CLIENT_SRC)

clean:
	rm -f $(SERVER) $(CLIENT)

run-server: $(SERVER)
	./$(SERVER)

run-client: $(CLIENT)
	./$(CLIENT)

.PHONY: all clean run-server run-client