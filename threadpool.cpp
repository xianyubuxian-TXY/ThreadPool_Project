#include "threadpool.h"
#include<functional>

const int TASK_MAX_THRESHHOLD =1024; //任务数量阈值
const int THREAD_MAX_THRESHHOLD =100; //线程数量阈值
const int THread_MAX_IDLE_TIME =60; //单位：秒（s）

//线程池构造
ThreadPool::ThreadPool()
    : initThreadSize_(0)
    , curThreadSize_(0)
    , idleThreadSize_(0)
    , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
    , taskSize_(0)
    , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
    , isPoolRunning_(false)
{}

//线程池析构
ThreadPool::~ThreadPool()
{
    isPoolRunning_=false;

    /* 如果先“唤醒”，再“lock”，仍存在死锁问题
    //唤醒所有等待状态的线程（用于回收等待状态的线程）
    notEmpty_.notify_all();
    //对于正在执行任务中的线程，在执行完任务后，发现isPoolRunning=false，会自动跳出while循环,在while循环外进行回收即可
    //等待线程池中所有的任务返回： 有两种状态： 阻塞 & 正在执行任务中
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    */
    
    //先锁定+“双重判断”
    //再“唤醒”
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    notEmpty_.notify_all();

    exitCond_.wait(lock,[&]()->bool{return threads_.size()==0;});
}

//设置线程模式
void ThreadPool::setMode(PoolMode mode){
    //运行后不可以再设置模式
    if(checkRunningState())
        return;
    poolMode_=mode;
}

//设置task任务队列阈值
void ThreadPool::setTaskQuemaxThreshHold(int threshhold){
    //运行后不可以再设置阈值
    if(checkRunningState())
        return;
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

    //cached模式：任务处理比较紧急  场景：小而快的任务， 需要根据任务数量和空闲线程的数量，判断是否需要增加/删除线程
    if(poolMode_==PoolMode::MDOE_ACHACED  //线程池工作在cached模式
        && taskSize_>idleThreadSize_      //任务数量大于空闲线程数量
        && curThreadSize_<threadSizeThreshHold_)  //当前线程池内线程数量小于线程数量阈值
    {
        std::cout<<"create new thread: "<<std::this_thread::get_id()<<std::endl;

        // 创建新线程对象
        auto ptr=std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this,std::placeholders::_1));
        //threads_.emplace_back(std::move(ptr));
        int threadId=ptr->getId();
            //注意：emplace与insert不同，emplace是以初值安插，insert是以拷贝安插
        threads_.emplace(threadId,std::move(ptr));
        //启动线程
        threads_[threadId]->start();
        //修改线程数量相关变量 
        curThreadSize_++;
        idleThreadSize_++;
    }
    //返回值
    //方式1：return task->getResult();——>不可以，因为随着task被执行完，task对象没了，依赖于task对象的result对象也没了
    //方式2：return Result(task);
    return Result(sp);
}

//开启线程池
void ThreadPool::start(int initThreadSize){
    //线程池启动
    isPoolRunning_=true;

    // 设置初始线程个数
    initThreadSize_=initThreadSize;
    curThreadSize_=initThreadSize_;

    //此次线程池线程的起始threadId(避免同时启动多个线程池出现错误)
    int firstThreadId=Thread::getGenerateId();

    // 创建线程对象
    for(int i=0;i<initThreadSize_;++i){
        //创建thread线程对象时，用“绑定器”将“线程函数”绑定为一个“函数对象”，然后传给thread线程对象
        //threadFunc()有参数“this”指针，通过bind()显示绑定this指针后，相当于没有参数
        auto ptr=std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this,std::placeholders::_1));
        //unique_ptr不可拷贝，只能”右值引用 move“
        //threads_.emplace_back(ptr);不行——>unique_ptr的”拷贝构造函数“=delete，在传入时会隐式调用其拷贝构造函数，故不行
        int threadId=ptr->getId();
        //注意：emplace与insert不同，emplace是以初值安插，insert是以拷贝安插
        threads_.emplace(threadId,std::move(ptr));
    }

    // 启动所有线程（与创建分开，让线程启动公平）
    for(int i=0;i<initThreadSize_;++i){
        //要提前记录firstThreadId，避免一个项目中启动多个线程池出现错误
        int threadId=firstThreadId+i;
        threads_[threadId]->start();
        idleThreadSize_++; //空闲线程数量+1：只是启动，还未分配任务
    }
}

//定义线程函数  线程池的所有线程从任务队列里面“消费任务“
void ThreadPool::threadFunc(int threadId){
    //线程上一次执行完任务的时间
    auto lastTime=std::chrono::high_resolution_clock().now();
   
    for(;;){
        std::shared_ptr<Task> task;
        {
            //先获取锁
            std::unique_lock<std::mutex> lock(taskQueMtx_); 
            
            std::cout<<"tid: "<<std::this_thread::get_id()<<" 尝试获取任务..."<<std::endl;
                           
            //没有任务时，轮询
            //双重判断isPoolRunning
            while(taskQue_.size()==0){

                if(!isPoolRunning_){
                    //执行完任务的线程发现isPoolRunning_=false：会自动跳出循环，进而进行回收
                    threads_.erase(threadId); //不能传入this_thread::get_id()
                    //修改线程数量相关变量
                    curThreadSize_--;
                    idleThreadSize_--;
                    //创建时使用this_thread::get_id,这里打印也就使用this_thread::get_id
                    std::cout<<"threadId: "<<std::this_thread::get_id()<<"exit!"<<std::endl;
                    exitCond_.notify_all();
                    return;//线程函数借宿线程结束
                }

                //cached模式下，有可能额外创建了很多线程，如果空闲时间超过60s，应该把多余的线程
                //结束回收掉（超过initThreadSize_数量的线程要进行回收）
                //当前时间 - 上一次线程执行的时间 > 60s
                if(poolMode_==PoolMode::MDOE_ACHACED)
                {
                    //每秒返回一次   怎么区分：超时返回？还是有任务待执行返回
                    //.wait_for()方法的返回值可以区分是否超时
                    if(std::cv_status::timeout ==notEmpty_.wait_for(lock,std::chrono::seconds(1)))
                    {
                        auto now=std::chrono::high_resolution_clock().now(); //获取当前时间
                        //通过std::chrono::duration_cast<std::chrono::seconds>强制类型转换为”秒“
                        auto dur=std::chrono::duration_cast<std::chrono::seconds>(now -lastTime);
                        //当前线程数量超出初始线程数量，且存在线程空闲时间超过60s，回收该线程
                        if( curThreadSize_>initThreadSize_ 
                            && dur.count()>=THread_MAX_IDLE_TIME)
                        {
                            //把线程从线程列表中删除(如何确定该线程是线程列表中哪个线程？给每个Thread一个id成员变量)
                                //线程不是有get_id函数吗，为什么还要手动分配？注意，Thread是我们对”线程“的封装类，并不是系统线程
                                //vector不方便删除，如何解决？map解决，正好配合threadId_
                                //如何获取threadId_?通过参数传入（因为threadFunc是ThreadPool的成员函数，而不是Thread的）
                            threads_.erase(threadId); //不能传入this_thread::get_id()
                            //修改线程数量相关变量
                            curThreadSize_--;
                            idleThreadSize_--;
    
                            //创建时使用this_thread::get_id,这里打印也就使用this_thread::get_id
                            std::cout<<"threadId: "<<std::this_thread::get_id()<<"exit!"<<std::endl;

                            return; //直接返回，退出for循环，线程结束
                        }
                    }
                }                
                else
                {
                    //等待notEmpty条件(fixed模式下，一直等待即可)
                    notEmpty_.wait(lock);
                }

                //如果被唤醒但处于”!isPoolRunning“状态——>由~ThreadPoool析构函数唤醒，则回收该线程（处理等待状态的线程）
                // if(!isPoolRunning_){
                //     threads_.erase(threadId); //不能传入this_thread::get_id()
                //     //修改线程数量相关变量
                //     curThreadSize_--;
                //     idleThreadSize_--;

                //     //创建时使用this_thread::get_id,这里打印也就使用this_thread::get_id
                //     std::cout<<"threadId: "<<std::this_thread::get_id()<<"exit!"<<std::endl;
                //     exitCond_.notify_all();
                //     return; //直接返回，退出for循环，线程结束
                // }
            }

            //执行任务
            idleThreadSize_--; //分配任务：空闲线程数量-1
            std::cout<<"tid: "<<std::this_thread::get_id()<<" 获取任务成功..."<<std::endl;

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
        idleThreadSize_++; //任务处理结束：空闲线程数量+1

        lastTime=std::chrono::high_resolution_clock().now();//更新线程执行完任务的时间

    }
}

bool ThreadPool::checkRunningState()const{
    return isPoolRunning_;
}

void ThreadPool::setThreadSizeThreshHold(int threshhold){
    //如果已经启动，则不可设置
    if(checkRunningState())
        return;
    //cached模式才可设置阈值，fixed没必要修改
    if(poolMode_==PoolMode::MDOE_ACHACED){
        threadSizeThreshHold_=threshhold;
    }

}

//////////////  Thread方法实现
int Thread::generateId_=0;

Thread::Thread(ThreadFunc func)
    : func_(func)
    , threadId_(generateId_++) //分配线程Id
{}

Thread::~Thread(){}

int Thread::getId() const{
    return threadId_;
}

int Thread::getGenerateId() {
    return generateId_;
}

//启动线程
void Thread::start(){
    //创建一个线程来执行一个线程函数
    std::thread t(func_,threadId_);  //创建线程对象
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