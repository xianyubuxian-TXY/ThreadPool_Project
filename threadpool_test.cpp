#include<iostream>
#include<chrono>
#include<thread>
#include "threadpool.h"

using uLong=unsigned long long;

class MyTask:public Task{
public:
    MyTask(int begin,int end):begin_(begin),end_(end){}
    Any run() override{
        std::cout<<"tid: "<<std::this_thread::get_id()<<" begin"<<std::endl;
        uLong sum=0;
        for(int i=begin_;i<=end_;i++){
            sum+=i;
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::cout<<"tid: "<<std::this_thread::get_id()<<" end "<<"sum="<<sum<<std::endl;
        return sum;
    }
private:
    int begin_;
    int end_;
};

int main(){
    {
        ThreadPool pool;

        pool.start(4);

        Result res1=pool.submitTask(std::make_shared<MyTask>(1,1000000000));
        uLong sum1=res1.get().cast_<uLong>();
        std::cout<<sum1<<std::endl;
        std::cout<<"main over!"<<std::endl;
    }


    return 0;
}