#ifndef THREADPOOL_H
#define THREADPOOL_H

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

//在实际开发中不要用using namespace std，防止“名空间污染”，直接用std::


//模板函数不可以是虚函数，那如何让虚函数返回值可以是任意类型？
class Any{
public:
    Any()=default;
    ~Any()=default;
    //因为是unique_ptr，所以将”拷贝构造“与”赋值构造“删除
    Any(const Any&)=delete;
    Any& operator=(const Any&)=delete;
    //提供“右值引用构造函数”
    Any(Any&&)=default;
    Any& operator=(Any&&)=default;

    //这个构造函数可以让Any类型接收任意其它的数据
    template<typename T>
    Any(T data): base_(std::make_unique<Derive<T>>(data)){}

    //这个方法能把Any对象里面存储的data数据提取出来
    template<typename T>
    T cast_(){
        //如何从base_找到它所指向的Derive对象，从它里面提取data_成员变量
        //“基类指针”转为“派生类指针”：dynamic_cast()强转
        Derive<T> *pd=dynamic_cast<Derive<T>*>(base_.get()); //智能指针的get方法：提取“裸指针”
        if(pd==nullptr){
            throw "type is unmatch!";
        }
        else{
            return pd->data_;
        }
    }
private:
    //基类类型
    class Base{
    public:
        virtual ~Base()=default;
    };

    //派生类类型(模板类)
    template<typename T>
    class Derive: public Base
    {
    public:
        Derive(T data):data_(data){}
        T data_; //保存了任意的其它类型
    };
private:
    //定义一个基类指针
    std::unique_ptr<Base> base_;
};


//实现一个信号量类：用于线程池的返回值（因为用户submit的线程与线程池内线程不同，用户获取返回值时任务可能还没有做完）
class Semaphore{
public:
    Semaphore(int limit=0):resLimit_(limit){}
    ~Semaphore()=default;
    //获取一个信号量
    void wait(){
        std::unique_lock<std::mutex> lock(mtx_);
        //这里先“-1”，是为了让resLimit的“绝对值”表示“被阻塞的线程数”（也可以先wait，在-1）
        // resLimit_--;
        // cond_.wait(lock,[&]()->bool{ return resLimit_>=0;});
        cond_.wait(lock,[&]()->bool{ return resLimit_>0;});
        resLimit_--;
    }
    //增加一个信号量
    void post(){
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        if(resLimit_>0){
            cond_.notify_all();
        }
    }
private:
    int resLimit_;
    std::mutex mtx_;
    std::condition_variable cond_;
};


//Task类的前置声明
class Task;

//实现接收提交到线程池
class Result{
public:
    Result(std::shared_ptr<Task> task,bool isValid=true);
    ~Result()=default;

    //setVal方法，获取任务执行完的返回值
    void setVal(Any any);  //线程池调用

    //get方法：用户调用这个方法获取task的返回值
    Any get(); //用户调用
private:
    Any any_; //存储任务的返回值
    Semaphore sem_; //线程通信的信号量
    std::shared_ptr<Task> task_; //指向对应获取返回值的任务对象
    std::atomic_bool isValid_; //返回值是否有效：如果用户任务提交失败，返回值则无效
};  

//任务抽象基类
class Task{
public:
    Task();
    ~Task()=default;
    //多态：用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
    //自定义Any类型，使虚函数可以返回各种类型（注意：不可以用模板，因为模板函数不可以是虚函数）
    virtual Any run()=0;

    void setResult(Result* result);

    void exec();
private:
    Result *result_; //注意：不可以用“强智能指针”，否则Task与Result会出现强智能指针的“交叉引用为题”
};

//线程池支持的模式
enum class PoolMode{
    MODE_FIXED, //固定数量的线程
    MDOE_CACHED, //线程数量可动态增长

};

class Thread{
public:
    //线程函数对象类型
    using ThreadFunc = std::function<void(int)>;
    
    Thread(ThreadFunc func);
    ~Thread();

    //启动线程
    void start();

    //获取线程id
    int getId() const;

    //获取generateId_
    static int getGenerateId();
private:
    ThreadFunc func_;
    static int generateId_; //确保每个线程id不同，用一个”静态成员“即可(类外初始化)
    int threadId_; //保存线程id
};

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
    ThreadPool();

    ~ThreadPool();

    //设置线程池的工作模式
    void setMode(PoolMode mode);

    //设置线程数量阈值
    void setThreadSizeThreshHold(int threshhold);
    
    //设置task任务队列阈值
    void setTaskQuemaxThreshHold(int threshhold);

    //给线程池提交任务
    Result submitTask(std::shared_ptr<Task> sp);

    //开启线程池(参数为初始线程数量,默认为4)
    //void start(int initThreadSize=4);
    //开启线程池(参数为初始线程数量,默认为"内核数量")
    void start(int initThreadSize=std::thread::hardware_concurrency());

    //线程池不允许“拷贝”与“复制”（成员太复杂了）
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    //定义线程函数：
    //1.线程由线程池创建，故线程能使用的函数由线程池提供
    //2.方便线程函数访问线程池中的变量
    void threadFunc(int threadId); //含有参数：this指针

    //检查pool的运行状态（可能多个地方调用，且都是内部方法）
    bool checkRunningState() const;
private:
    //池内线程相关
    // std::vector<std::unique_ptr<Thread>> threads_; //线程列表
    std::unordered_map<int,std::unique_ptr<Thread>> threads_; //线程列表
    std::size_t initThreadSize_; //初始线程数量 
    std::atomic_int curThreadSize_; //当前线程池中的总数量
    std::atomic_int idleThreadSize_; //空闲线程数量(cached模式使用)
    int threadSizeThreshHold_; //线程数量的阈值(cached模式才可设置)

    //池内任务相关
    std::queue<std::shared_ptr<Task>> taskQue_; //任务队列
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