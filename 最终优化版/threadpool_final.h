#ifndef THREADPOOL_FINAL_H
#define THREADPOOL_FINAL_H

#include<iostream>
#include<vector>
#include<queue>
#include<unordered_map>
#include<memory>
#include<atomic>
//线程中的线程与用户提交的线程会同时操作任务队列，所以需要线程的“互斥”、“同步”机制
#include<mutex>
#include<condition_variable> //条件变量：“线程通信”
#include<functional>  //bind()
#include<thread>
#include<future>

const int TASK_MAX_THRESHHOLD =2; //任务数量阈值
const int THREAD_MAX_THRESHHOLD =100; //线程数量阈值
const int THread_MAX_IDLE_TIME =60; //单位：秒（s）

enum class PoolMode
{
    MODE_FIXED,  //固定线程数量
    MODE_CACHED, //线程数量可动态增长
};

class Thread{
public:
    //线程函数对象类型
    using ThreadFunc = std::function<void(int)>;
    
    Thread(ThreadFunc func)
        : func_(func)
        , threadId_(generateId_++) //分配线程Id
    {}

    ~Thread()=default;

    //启动线程
    void start()
    {
        //创建一个线程来执行一个线程函数
        std::thread t(func_,threadId_);  //创建线程对象
        t.detach(); //设置分离线程，因为t是局部变量，出了start()作用域就要“析构”
    }

    //获取线程id
    int getId() const
    {
        return threadId_;
    }

    //获取generateId_
    static int getGenerateId()
    {
        return generateId_;
    }
private:
    ThreadFunc func_;
    static int generateId_; //确保每个线程id不同，用一个”静态成员“即可(类外初始化)
    int threadId_; //保存线程id
};

int Thread::generateId_=0;


/*
example:
ThreadPool pool;
pool.setMode(PoolMode::MODE_FIXED |PoolMode::MDOE_ACHACED); //默认为fixed模式
pool.start(4);

class MyTask:public Task{
public:
    void run() override{
        //线程代码
    }

};

pool.submitTask(std::shared_ptr<MyTask>());

*/
class ThreadPool{
public:
    ThreadPool()
        : initThreadSize_(0)
        , curThreadSize_(0)
        , idleThreadSize_(0)
        , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
        , taskSize_(0)
        , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
        , poolMode_(PoolMode::MODE_FIXED)
        , isPoolRunning_(false)
    {}

    ~ThreadPool()
    {
        isPoolRunning_=false;

        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();
    
        exitCond_.wait(lock,[&]()->bool{return threads_.size()==0;});
    }

    //设置线程池的工作模式
    void setMode(PoolMode mode)
    {
        if(checkRunningState())
            return;
        poolMode_=mode;
    }

    //设置task任务队列阈值
    void setTaskQuemaxThreshHold(int threshhold)
    {
        if(checkRunningState())
            return;
        taskQueMaxThreshHold_=threshhold;
    }

    void setThreadSizeThreshHold(int threshhold){
        //如果已经启动，则不可设置
        if(checkRunningState())
            return;
        //cached模式才可设置阈值，fixed没必要修改
        if(poolMode_==PoolMode::MODE_CACHED){
            threadSizeThreshHold_=threshhold;
        }
    }
    


    //给线程池提交任务
    //使用可变参模板编程，让SubmitTask可以接收任意任务函数和任意数量的参数
    //返回值future<>但不知道具体类型怎么办？
    // Result submitTask(std::shared_ptr<Task> sp);
    template<typename Func,typename... Args>  //Func：函数类型   Args...:参数包 
    auto submitTask(Func&& func,Args&&... args)->std::future<decltype(func(args...))>
    {
        //打包任务，放入任务队列
        using RType=decltype(func(args...));
        //packaged_task<RType()>：返回一个“返回值类型为RType，无参数”的函数对象——>因为用了std::bind,所以不需要参数
        auto task=std::make_shared<std::packaged_task<RType()>>(
                    std::bind(std::forward<Func>(func),std::forward<Args>(args)...));
        std::future<RType> result=task->get_future();
        
        //获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        //条件不满足，最多阻塞1秒，超过1秒则提交失败
        if(!notFull_.wait_for(lock,std::chrono::seconds(1),[&]()
            ->bool{ return taskQue_.size()<taskQueMaxThreshHold_;}))
        {
            //notFull_等待1秒，条件还是不满足
            std::cerr<<"task queue is full,submit task fail."<<std::endl;
            
            //任务提交失败，通过packaged_task创建一个临时“函数对象”，配合get_future返回一个RType类型的默认值RType()
            auto task=std::make_shared<std::packaged_task<RType()>>([]()->RType{ return RType();});
            (*task)();
            return task->get_future();
        }
        //如果有空余，把任务放入任务队列中
        taskQue_.emplace([task](){ (*task)(); });
        taskSize_++;
        //因为有新任务，任务队列肯定不空，在notEmpty_上进行通知,分配线程执行任务
        notEmpty_.notify_all();

        //cached模式：任务处理比较紧急  场景：小而快的任务， 需要根据任务数量和空闲线程的数量，判断是否需要增加/删除线程
        if(poolMode_==PoolMode::MODE_CACHED //线程池工作在cached模式
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

        return result;
    }

    //开启线程池(参数为初始线程数量,默认为4)
    //void start(int initThreadSize=4);
    //开启线程池(参数为初始线程数量,默认为"内核数量")
    void start(int initThreadSize=std::thread::hardware_concurrency())
    {
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

    //线程池不允许“拷贝”与“复制”（成员太复杂了）
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    //定义线程函数：
    //1.线程由线程池创建，故线程能使用的函数由线程池提供
    //2.方便线程函数访问线程池中的变量
    void threadFunc(int threadId) //含有参数：this指针
    {
        //线程上一次执行完任务的时间
        auto lastTime=std::chrono::high_resolution_clock().now();
    
        for(;;){
            Task task;
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
                    if(poolMode_==PoolMode::MODE_CACHED)
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
                task();
            }
            idleThreadSize_++; //任务处理结束：空闲线程数量+1

            lastTime=std::chrono::high_resolution_clock().now();//更新线程执行完任务的时间

        }
    }

    //检查pool的运行状态（可能多个地方调用，且都是内部方法）
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }
private:
    //池内线程相关
    // std::vector<std::unique_ptr<Thread>> threads_; //线程列表
    std::unordered_map<int,std::unique_ptr<Thread>> threads_; //线程列表
    std::size_t initThreadSize_; //初始线程数量 
    std::atomic_int curThreadSize_; //当前线程池中的总数量
    std::atomic_int idleThreadSize_; //空闲线程数量(cached模式使用)
    int threadSizeThreshHold_; //线程数量的阈值(cached模式才可设置)

    //池内任务相关
    using Task=std::function<void()>;
    std::queue<Task> taskQue_; //任务队列
    std::atomic_uint taskSize_; //任务数量（因为是动态的，可能发送“竞争”，所以用“原子类型”）
    int taskQueMaxThreshHold_; //任务队列数量上线阈值（因为不会变，所以用普通类型即可）

    //池内安全相关
    std::mutex taskQueMtx_; //保证任务队列的线程安全
    std::condition_variable notFull_; //表示任务队列不满
    std::condition_variable notEmpty_;  //表示任务队列不空
    std::condition_variable exitCond_; //等待线程资源全部回收

    //线程池状态
    PoolMode poolMode_; //当前线程池的工作模式
    std::atomic_bool isPoolRunning_; //当前线程是否已经开始（开始后不允许在设置Mode）
};

#endif 