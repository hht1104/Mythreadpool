#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>//线程通信
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;//单位：s

//线程池支持的模式
enum class PoolMode
{
	MODE_FIXED,//固定数量的线程
	MODE_CACHED,//线程数量可动态增长
};
//线程类型
class Thread
{
public:
	//线程函数对象类型
	using ThreadFunc = std::function<void(int)>;

	//线程构造
	Thread(ThreadFunc func)
		:func_(func)
		, threadId_(generateId_++)
	{}

	//线程析构
	~Thread() = default;
	
	//启动线程
	void start()
	{
		//创建一个线程来执行一个线程函数
		std::thread t(func_, threadId_);//C++11来说  线程对象t 和线程函数func_
		t.detach();//设置分离线程 pthread_datach   pthread_t设置成分离线程
	}

	//获取线程id
	int getId()const
	{
		return threadId_;
	}

private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;//保持线程id
};

int Thread::generateId_ = 0;

//线程池类型
class ThreadPool
{
public:
	//线程池构造
	ThreadPool()
		: initThreadSize_(0)
		, taskSize_(0)
		, idleThreadSize_(0)
		, curThreadSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
	{}

	//线程池析构
	~ThreadPool()
	{
		isPoolRunning_ = false;

		//等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中 
		std::unique_lock <std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}

	//设置线程池工作模式
	void setMode(PoolMode mode) 
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}

	//设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	//设置线程池cached模式下线程阈值
	void setThreadSizeThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshHold_ = threshhold;
		}
	}

	//给线程池提交任务
	//使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
	//pool.submitTask(sum1, 10, 20);
	//返回值future<>
	template<typename Func,typename ... Args>//引用折叠  可变参模板编程
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>//函数返回值的类型推导
	{
		//打包任务，放入任务队列里
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();

		//获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//用户提交任务，最长不能阻塞超过1s，否则判断任务提交失败，返回
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
		{
			//表示notFull_等待1s后，条件依然没有满足
			std::cerr << "task queue is full,submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType {return RType(); });
			(*task)();
			return task->get_future();
		}

		//如果有空余把任务队列放入任务中
		//taskQue_.emplace(sp);
		//using Task = std::function<void()>;
		//去执行下面的任务
		taskQue_.emplace([task]() {(*task)();});//解引用
		taskSize_++;

		//因为新放了任务，任务队列肯定不空了，notEmpty_通知，赶快分配线程执行任务
		notEmpty_.notify_all();

		//cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务的数量和空闲线程的数量，判断是否需要创建新的线程出来
		if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
		{
			std::cout << ">>> create new thread..." << std::endl;

			//创建新线程对象
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));

			//启动线程
			threads_[threadId]->start();

			//修改线程个数相关的变量
			curThreadSize_++;
			idleThreadSize_++;
		}

		//返回任务的Result对象
		return result;
	}
		
	//开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency())//返回当前CPU核心数量
	{
		//设置线程池的运行状态
		isPoolRunning_ = true;

		//记录初始线程个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		//创建线程对象
		for (int i = 0; i < initThreadSize_; i++)
		{
			//创建thread线程对象的时候，把线程函数给到thread线程对象
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			//threads_.emplace_back(std::move(ptr));
		}

		//启动所有线程    std::vector<Thread*>threads_;//线程列表
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start();//需要去执行一个线程函数
			idleThreadSize_++;//记录初始空闲线程数量
		}
	}
	ThreadPool(const ThreadPool&) = delete;//杜绝用户直接使用线程池
	ThreadPool& operator = (const ThreadPool&) = delete;

private:
	//定义线程函数
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		//所有任务必须执行完成，线程池才能回收所有的线程资源
		for (;;)
		{
			Task task;
			{
				//先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid:" << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

				//3.在cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程(超过initThreadSize_数量的线程)结束回收掉
				//当前时间 — 上次线程执行的时间 > 60s
				// 
				//每一秒钟返回一次  怎么区分：超时返回   有任务待执行返回
				//锁 + 双重判断
				while (taskQue_.size() == 0)
				{
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);//std::this_thread::getid()
						std::cout << "threadid:" << std::this_thread::get_id() << "exit!" << std::endl;
						exitCond_.notify_all();
						return;//线程函数结束，就是线程结束
					}

					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						//条件变量，超时返回
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))//阻塞1s
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
							{
								//开始回收当前线程
								//记录线程数量的相关变量的值修改
								//把线程对象从线程对象列表容器中删除  没有办法threadFunc对应的哪一个thread对象
								//threadid=> thread 对象=> 删除
								threads_.erase(threadid);//std::this_thread::getid()
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "threadid:" << std::this_thread::get_id() << "exit!" << std::endl;
								return;
							}
						}
					}
					else
					{
						//等待notEmpty条件
						notEmpty_.wait(lock);
					}
				}

				idleThreadSize_--;

				std::cout << "tid:" << std::this_thread::get_id() << "获取任务成功..." << std::endl;

				//从任务队列中取一个任务出来
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				//如果依然有剩余任务，继续通知其他线程执行任务
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				//取出一个任务，进行通知，通知可以继续提交生产任务
				notFull_.notify_all();

			}//应该释放掉锁

			 //当前线程负责执行这个任务
			if (task != nullptr)
			{
				task();//执行function<void()>
			}

			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now();//更新线程执行完任务的时间
		}
	}

	//检查pool的运行状态
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;//线程列表

	int initThreadSize_;//初始的线程数量
	int threadSizeThreshHold_;//线程数量上限阈值
	std::atomic_int curThreadSize_;//记录当前线程池里线程总数量
	std::atomic_int idleThreadSize_;//记录空闲线程的数量

	//Task任务=》函数对象
	using Task = std::function<void()>;//中间层
	std::queue<Task>taskQue_;//任务队列    智能指针 能保持拉长对象的生命周期，自动释放资源
	std::atomic_int taskSize_;//任务数量
	int taskQueMaxThreshHold_;//任务队列数量上限阈值

	std::mutex taskQueMtx_;//保证任务队列的线程安全   互斥锁
	std::condition_variable notFull_; //条件变量   表示任务队列不满
	std::condition_variable notEmpty_;//条件变量   表示任务队列不空
	std::condition_variable exitCond_;//等待线程资源全部回收

	PoolMode poolMode_;//记录当前线程池的工作模式
	std::atomic_bool isPoolRunning_;//表示当前线程池的启动状态
};

#endif
