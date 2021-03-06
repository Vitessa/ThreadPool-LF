#pragma once

#include <limits>
#include <vector>
#include <queue>
#include <future>
#include <functional>

namespace vitessa
{

class CThreadPool
{
public:
    // 创建一个只有一个线程的线程池
    CThreadPool();

    // 创建一个有n个线程的线程池
    CThreadPool(unsigned char thread_num);

    // 销毁线程池，会阻塞直到所有任务完成
    ~CThreadPool();

    // 向线程池中添加任务，线程安全
    //     \fx  要执行的函数
    //     \ax  函数的参数
    //     \rt  返回future<return_type>
    template <class Fn, class... Args>
    std::future<typename std::result_of<Fn(Args...)>::type> spawn(Fn&& fx, Args&&... ax);

private:
    // 禁止拷贝
    CThreadPool(const CThreadPool& rhs) = delete;
    // 线程原型
    static void route(CThreadPool* tp);

private:
    std::vector<std::thread> _vecThread;             // 线程组
    std::queue< std::function<void()> > _queTask;    // 任务队列
    std::mutex _mtx;                                 // 互斥锁
    bool _isStop;                                    // 是否停止（析构函数用）
    std::condition_variable _condFollower;
    std::condition_variable _condLeader;
    std::thread::id _leaderId;
}; 

class CPrioThreadPool
{
    struct GreaterPrioTask;
    using prio_t = unsigned char;
    using PrioTask = std::tuple< prio_t, unsigned long long, std::function<void()> >;
    using QuePrioTask = std::priority_queue< PrioTask, std::vector<PrioTask>, GreaterPrioTask >;

    struct GreaterPrioTask
    {
        bool operator() (const PrioTask& lhs, const PrioTask& rhs) const
        {
            return std::get<0>(lhs) == std::get<0>(rhs) ?
                   std::get<1>(lhs) > std::get<1>(rhs) :
                   std::get<0>(lhs) > std::get<0>(rhs) ;
        }
    };

public:
    // 创建一个只有一个线程的线程池
    CPrioThreadPool();

    // 创建一个有n个线程的线程池
    CPrioThreadPool(unsigned char thread_num);

    // 销毁线程池，会阻塞直到所有任务完成
    ~CPrioThreadPool();

    // 向线程池中添加任务，线程安全
    //     \prio  任务优先级，0为最高优先级，255为最低优先级
    //     \fx    要执行的函数
    //     \ax    函数的参数
    //     \rt    返回future<return_type>
    template <class Fn, class... Args>
    std::future<typename std::result_of<Fn(Args...)>::type> spawn(prio_t prio, Fn&& fx, Args&&... ax);

    // 添加优先级为0的任务，线程安全
    //     \fx       要执行的函数
    //     \ax       函数的参数
    //     \rt       返回future<return_type>
    template <class Fn, class... Args>
    std::future<typename std::result_of<Fn(Args...)>::type> spawn_front(Fn&& fx, Args&&... ax);

    // 添加优先级为255的任务，线程安全
    //     \fx       要执行的函数
    //     \ax       函数的参数
    //     \rt       返回future<return_type>
    template <class Fn, class... Args>
    std::future<typename std::result_of<Fn(Args...)>::type> spawn_back(Fn&& fx, Args&&... ax);

private:
    // 禁止拷贝
    CPrioThreadPool(const CPrioThreadPool& rhs) = delete;
    // 线程原型
    static void route(CPrioThreadPool* tp);

private:
    std::vector<std::thread> _vecThread;             // 线程组
    QuePrioTask _quePrioTask;                        // 任务队列
    std::mutex _mtx;                                 // 互斥锁
    bool _isStop;                                    // 是否停止（析构函数用）
    std::condition_variable _condFollower;
    std::condition_variable _condLeader;
    std::thread::id _leaderId;
    unsigned long long _cnt = 0;
};


inline CThreadPool::CThreadPool() : _isStop(false)
{
    _vecThread.emplace_back( CThreadPool::route, this );
}

inline CThreadPool::CThreadPool(unsigned char thread_num) : _isStop(false)
{
    for (unsigned char i = 0; i < thread_num; ++ i) {
        _vecThread.emplace_back( CThreadPool::route, this );
    }
}

inline CThreadPool::~CThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(_mtx);
        _isStop = true;
    }
    _condFollower.notify_all();
    _condLeader.notify_all();
    for (std::thread& t : _vecThread) {
        t.join();
    }
}

template <class Fn, class... Args>
inline std::future<typename std::result_of<Fn(Args...)>::type> CThreadPool::spawn(Fn&& fx, Args&&... ax)
{
    using return_type = typename std::result_of<Fn(Args...)>::type;
    auto task = std::make_shared< std::packaged_task<return_type()> > (
        std::bind(std::forward<Fn>(fx), std::forward<Args>(ax)...) );

    std::future<return_type> res = task->get_future();

    {
        std::unique_lock<std::mutex> lock(this->_mtx);
        if (this->_isStop) {
            throw std::runtime_error("spawn a stoped thread pool.");
        }
        this->_queTask.emplace([task](){(*task)();});
    }

    this->_condLeader.notify_one();
    return res;
}

inline void CThreadPool::route(CThreadPool* tp)
{
    std::function<void()> task;
    for (;;)
    {
        {   // lock_guard start
            std::unique_lock<std::mutex> lock(tp->_mtx);
            if (tp->_leaderId == std::thread::id()) { // Leader 
                tp->_leaderId = std::this_thread::get_id();
                tp->_condLeader.wait(lock, [tp]{return tp->_isStop || !tp->_queTask.empty();});
                if (tp->_queTask.empty()) {
                    return;
                }
                tp->_leaderId = std::thread::id();
                task = std::move(tp->_queTask.front());
                tp->_queTask.pop();
            }
            else { // Follower
                tp->_condFollower.wait(lock, [tp]{return tp->_isStop || tp->_leaderId == std::thread::id();});
                if (tp->_isStop && tp->_queTask.empty()) {
                    return;
                }
                continue;
            }
        }   // lock_guard end

        //Call Follower to become Leader
        tp->_condFollower.notify_one();
        // Worker
        task();
    }
}

inline CPrioThreadPool::CPrioThreadPool() : _isStop(false)
{
     _vecThread.emplace_back( CPrioThreadPool::route, this );
}

inline CPrioThreadPool::CPrioThreadPool(unsigned char thread_num) : _isStop(false)
{
    for (unsigned char i = 0; i < thread_num; ++ i) {
        _vecThread.emplace_back( CPrioThreadPool::route, this );
    }
}

inline CPrioThreadPool::~CPrioThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(_mtx);
        _isStop = true;
    }
    _condFollower.notify_all();
    _condLeader.notify_all();
    for (std::thread& t : _vecThread) {
        t.join();
    }
}

template <class Fn, class... Args>
inline std::future<typename std::result_of<Fn(Args...)>::type> CPrioThreadPool::spawn(prio_t prio, Fn&& fx, Args&&... ax)
{
    using return_type = typename std::result_of<Fn(Args...)>::type;
    auto task = std::make_shared< std::packaged_task<return_type()> > (
        std::bind(std::forward<Fn>(fx), std::forward<Args>(ax)...) );

    std::future<return_type> res = task->get_future();

    {
        std::unique_lock<std::mutex> lock(this->_mtx);

        if (this->_isStop) {
            throw std::runtime_error("spawn a stoped thread pool.");
        }

        this->_quePrioTask.emplace(prio, ++_cnt, [task](){(*task)();});
    }

    this->_condLeader.notify_one();
    return res;
}

template <class Fn, class... Args>
inline std::future<typename std::result_of<Fn(Args...)>::type> CPrioThreadPool::spawn_front(Fn&& fx, Args&&... ax)
{
    return this->spawn((std::numeric_limits<prio_t>::min)(), fx, std::forward<Args>(ax)...);
}

template <class Fn, class... Args>
inline std::future<typename std::result_of<Fn(Args...)>::type> CPrioThreadPool::spawn_back(Fn&& fx, Args&&... ax)
{
    return this->spawn((std::numeric_limits<prio_t>::max)(), fx, std::forward<Args>(ax)...);
}

inline void CPrioThreadPool::route(CPrioThreadPool* tp)
{
    std::function<void()> task;
    for (;;)
    {
        {   // lock_guard start
            std::unique_lock<std::mutex> lock(tp->_mtx);
            if (tp->_leaderId == std::thread::id()) { // Leader 
                tp->_leaderId = std::this_thread::get_id();
                tp->_condLeader.wait(lock, [tp]{return tp->_isStop || !tp->_quePrioTask.empty();});
                if (tp->_quePrioTask.empty()) {
                    return;
                }
                tp->_leaderId = std::thread::id();
                task = std::move(std::get<2>(tp->_quePrioTask.top()));
                tp->_quePrioTask.pop();
            }
            else { // Follower
                tp->_condFollower.wait(lock, [tp]{return tp->_isStop || tp->_leaderId == std::thread::id();});
                if (tp->_isStop && tp->_quePrioTask.empty()) {
                    return;
                }
                continue;
            }
        }   // lock_guard end

        //Call Follower to become Leader
        tp->_condFollower.notify_one();
        // Worker
        task();
    }
}

}

