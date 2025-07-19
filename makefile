all: threadpool_test

threadpool_test: threadpool.h threadpool.cpp threadpool_test.cpp
	g++ -o threadpool_test threadpool.cpp threadpool_test.cpp -pthread

clean:
	rm -f threadpool_test
