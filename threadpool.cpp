#include "threadpool.h"
#include<functional>

const int TASK_MAX_THRESHHOLD =1024;

//线程池构造
ThreadPool::ThreadPool()
    : initThreadSize_(4)
    , taskSize_(0)
    , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
{}

//线程池析构
ThreadPool::~ThreadPool()
{}

//设置线程模式
void ThreadPool::setMode(PoolMode mode){
    poolMode_=mode;
}

//设置task任务队列阈值
void ThreadPool::setTaskQuemaxThreshHead(int threshhold){
    taskQueMaxThreshHold_=threshhold;
}

//给线程池提交任务 用户调用该接口传入任务对象，“生成任务”
Result ThreadPool::submitTask(std::shared_ptr<Task> sp){
    //获取锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    //线程的通信 等待任务队列有空余
    // while(taskQue_.size()==taskQueMaxThreshHold_)
    // {
    //     notFull_.wait(lock);
    // }
    //条件不满足，最多阻塞1秒，超过1秒则提交失败
    if(!notFull_.wait_for(lock,std::chrono::seconds(1),[&]()
        ->bool{ return taskQue_.size()<taskQueMaxThreshHold_;})){
        //notFull_等待1秒，条件还是不满足
        std::cerr<<"task queue is full,submit task fail."<<std::endl;
        //任务提交失败：“返回值无效”
        return Result(sp,false);
    }
    //如果有空余，把任务放入任务队列中
    taskQue_.emplace(sp);
    taskSize_++;
    //因为有新任务，任务队列肯定不空，在notEmpty_上进行通知,分配线程执行任务
    notEmpty_.notify_all();

    //返回值
    //方式1：return task->getResult();——>不可以，因为随着task被执行完，task对象没了，依赖于task对象的result对象也没了
    //方式2：return Result(task);
    return Result(sp);
}

//开启线程池
void ThreadPool::start(int initThreadSize){
    // 设置初始线程个数
    initThreadSize_=initThreadSize;

    // 创建线程对象
    for(int i=0;i<initThreadSize_;++i){
        //创建thread线程对象时，用“绑定器”将“线程函数”绑定为一个“函数对象”，然后传给thread线程对象
        //threadFunc()有参数“this”指针，通过bind()显示绑定this指针后，相当于没有参数
        auto ptr=std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this));
        //unique_ptr不可拷贝，只能”右值引用 move“
        //threads_.emplace_back(ptr);不行——>unique_ptr的”拷贝构造函数“=delete，在传入时会隐式调用其拷贝构造函数，故不行
        threads_.emplace_back(std::move(ptr));
    }

    // 启动所有线程（与创建分开，让线程启动公平）
    for(int i=0;i<initThreadSize_;++i){
        threads_[i]->start();
    }
}

//定义线程函数  线程池的所有线程从任务队列里面“消费任务“
void ThreadPool::threadFunc(){
    // std::cout<<"begin threadFunc tid:"<<std::this_thread::get_id()<<std::endl;
    // std::cout<<"end threadFunc tid:"<<std::this_thread::get_id()<<std::endl;
    //死循环：不断执行
    for(;;){
        std::shared_ptr<Task> task;
        {
            //先获取锁
            std::unique_lock<std::mutex> lock(taskQueMtx_); 
            std::cout<<"tid: "<<std::this_thread::get_id()<<" 尝试获取任务..."<<std::endl;
            //等待notEmpty条件
            notEmpty_.wait(lock,[&]()->bool{return taskQue_.size() > 0;});
            std::cout<<"tid: "<<std::this_thread::get_id()<<" 尝试任务成功..."<<std::endl;
            //从任务队列中取一个任务出来
            task=taskQue_.front();
            taskQue_.pop();
            taskSize_--;

            //如果依然有剩余任务，通知另一个线程执行任务
            if(taskQue_.size()>0){
                notEmpty_.notify_all();
            }

            //取出任务后，notFull_进行通知
            notFull_.notify_all();
            
            //右括号：调用”析构函数“——>释放掉锁（一定要在执行前释放，否则在执行前都不释放锁，变为串行）
        }

        //当前线程执行该任务
        //条件变量可能发生”假醒“——>苏醒之后要再次检查条件
        if(task!=nullptr){
            task->exec();
        }
    }
}


//////////////  Thread方法实现
Thread::Thread(ThreadFunc func)
    : func_(func)   
{}

Thread::~Thread(){}

//启动线程
void Thread::start(){
    //创建一个线程来执行一个线程函数
    std::thread t(func_);  //创建线程对象
    t.detach(); //设置分离线程，因为t是局部变量，出了start()作用域就要“析构”
}

/////////////  Task方法实现
Task::Task():result_(nullptr){}

void Task::setResult(Result* result){
    result_=result;
}

void Task::exec(){
    //指针做一下判断，更加安全
    if(result_!=nullptr){
        result_->setVal(run()); //这里发生多态调用
    }
}

////////////////// Result方法实现
Result::Result(std::shared_ptr<Task> task,bool isValid)
    :task_(task)
    ,isValid_(isValid)
{
    task_->setResult(this);
}

Any Result::get(){
    if(!isValid_){
        return nullptr;
    }
    sem_.wait(); //task任务如果没有执行完，这里会阻塞任务的线程
    return std::move(any_);
}

void Result::setVal(Any any){
    //存储task的返回值
    this->any_=std::move(any);
    //已经获取的任务的返回值，增加信号量资源
    sem_.post();
}