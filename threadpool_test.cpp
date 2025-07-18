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
        pool.setMode(PoolMode::MDOE_ACHACED);
        pool.start(4);
        Result result1=pool.submitTask(std::make_shared<MyTask>(1,100000000));
        Result result2=pool.submitTask(std::make_shared<MyTask>(100000001,200000000));
        Result result3=pool.submitTask(std::make_shared<MyTask>(200000001,300000000));
        pool.submitTask(std::make_shared<MyTask>(200000001,300000000));
        pool.submitTask(std::make_shared<MyTask>(200000001,300000000));
        pool.submitTask(std::make_shared<MyTask>(200000001,300000000));
    
        uLong sum1=result1.get().cast_<uLong>();
        uLong sum2=result2.get().cast_<uLong>();
        uLong sum3=result3.get().cast_<uLong>();
    
        //Master-Slave线程模型
        //Master线程用来分解任务，然后给各个Slave线程分配任务
        //等待各个Slave线程执行任务，返回结果
        //Master线程合并各个任务结果，输出结果
        std::cout<<(sum1+sum2+sum3)<<std::endl;
    }


    return 0;
}