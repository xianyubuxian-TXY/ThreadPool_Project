all: threadpool_test 

#生成测试文件
threadpool_test: threadpool.h threadpool.cpp threadpool_test.cpp
	g++ -o threadpool_test threadpool.cpp threadpool_test.cpp -lpthread -g
clean:
	rm -f threadpool_test
