CFLAGS = -Wall --std=c++20

all: server client

clear:
	rm server client

server: server.cpp common.h request.h
	g++ $(CFLAGS) server.cpp -o server

client: client.cpp common.h request.h
	g++ $(CFLAGS) client.cpp -o client