CC=g++
CFLAGS=-g -std=c++17 -Wall -O3  -lnuma -lpthread -DNDEBUG
TARGET=./bin/lpht

all:
	$(CC)  main.cpp -o $(TARGET) $(CFLAGS) city/city.cc

clean:
	rm -f $(TARGET) *.o
