#include <iostream>
#include <chrono>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <functional>
#include <atomic>
#include <random>
#include <condition_variable>

#include <algorithm> // DELETE DELETE DELETE DELETE
#include <cassert> // DELETE DELETE DELETE DELETE


#define THREAD_COUNT 2
#define EXECUTION_INTERVAL 30
#define MIN_TASK_TIME 2
#define MAX_TASK_TIME 8

using namespace std;
using namespace chrono;

using read_lock = shared_lock<shared_mutex>;
using write_lock = unique_lock<shared_mutex>;

atomic<size_t> prev_id = 0;

class task
{

public:
    task() : task_id(prev_id++), task_time(random_time()) {};
    task(size_t id, seconds time) : task_id(id), task_time(time) {}
    ~task() = default;

    size_t get_id() const { return task_id; }
    seconds get_time() const { return task_time; }

    void execute()
    {
        this_thread::sleep_for(task_time);
    }

private:
    size_t task_id;
    seconds task_time;

    seconds random_time()
    {
        static thread_local mt19937 generator(random_device{}());
        uniform_int_distribution<int> distribution(MIN_TASK_TIME, MAX_TASK_TIME);
        return seconds(distribution(generator));
    }
};

class task_queue
{

public:
    task_queue() = default;
    ~task_queue() {clear();};

    bool empty()
    {

        read_lock _(rw_lock);
        return tasks.empty();
    }

    size_t size()
    {
        read_lock _(rw_lock);
        return tasks.size();
    }

    void clear()
    {
        write_lock _(rw_lock);
        while (!tasks.empty())
        {
            tasks.pop();
        }
    }

    bool pop(task &task)
    {
        write_lock _(rw_lock);
        if (tasks.empty())
        {
            return false;
        }

        else
        {
            task = move(tasks.front());
            tasks.pop();
            return true;
        }
    }

    void emplace(task &&task)
    {
        write_lock _(rw_lock);
        tasks.emplace(move(task));
    }

private:
    queue<task> tasks;
    mutable shared_mutex rw_lock;
};

class thread_pool
{

public:
    thread_pool() = default;
    ~thread_pool() { terminate(); };

    bool working()
    {
        read_lock _(rw_lock);
        return working_unsafe();   
    }

    bool working_unsafe()
    {
        return initialized && !terminated;
    }

    void initialize(size_t worker_count)
    {
        write_lock _(rw_lock);
        
        if (initialized || terminated)
        {
            return;
        }

        workers.reserve(worker_count);
        for (size_t id = 0; id < worker_count; id++)
        {
            workers.emplace_back(routine, this);
        }

        initialized = !workers.empty();
    }

    void routine()
    {
        while(true)
        {
            bool task_accquired = false;

            task task;

            {
                write_lock _(rw_lock);
                auto wait_condition = [this, &task_accquired, &task] 
                {
                    task_accquired = tasks.pop(task);
                    return terminated || task_accquired;
                };

                task_waiter.wait(_, wait_condition);
            }

            if (terminated && !task_accquired)
            {
                return;
            }

            task.execute();
        }
    }

    void add_task(task &&task)
    {
        {
            read_lock _(rw_lock);
            if (!working_unsafe())
            {
                return;
            }
        }

        {
            write_lock _(rw_lock);
            tasks.emplace(move(task));
        }

        task_waiter.notify_one();
    }

    void terminate()
    {
        {
            write_lock _(rw_lock);
            if(working_unsafe())
            {
                terminated = true;
            }

            else{
                workers.clear();
                terminated = false;
                initialized = false;
                return;
            }
        }

        task_waiter.notify_all();

        for (thread &worker : workers)
        {
            worker.join();
        }

        workers.clear();
        terminated = false;
        initialized = false;
    }    

private:

    bool initialized = false;
    bool terminated = false;

    task_queue tasks;

    vector<thread> workers;

    mutable shared_mutex rw_lock;
    mutable condition_variable_any task_waiter;
};

int main()
{

    return 0;
}