CXX = g++
CXXFLAGS = -Wall -Wextra -pthread -O2 -Wno-stringop-truncation -Wno-unused-parameter -Wno-format-truncation
LDFLAGS = -pthread
SSLFLAGS = -lssl -lcrypto

TARGETS = server client
OBJS = 

all: $(TARGETS)

server: server.cpp structs.h
	$(CXX) $(CXXFLAGS) -o server server.cpp $(LDFLAGS) $(SSLFLAGS)

client: client.cpp structs.h
	$(CXX) $(CXXFLAGS) -o client client.cpp $(LDFLAGS) $(SSLFLAGS)

cert:
	@echo "Generating self-signed certificate..."
	openssl req -x509 -newkey rsa:2048 \
		-keyout server.key \
		-out server.crt \
		-days 365 \
		-nodes \
		-subj "/C=RU/ST=Moscow/L=Moscow/O=University/CN=localhost"
	@echo "Certificate generated: server.crt"
	@echo "Private key generated: server.key"
	@chmod 600 server.key

run-server: server
	./server

run-server-sim: server
	./server --delay=100 --drop=0.2 --corrupt=0.1

run-client: client
	./client

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean