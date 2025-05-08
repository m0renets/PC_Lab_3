#include <iostream>
#include <chrono>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include <cassert> // DELETE DELETE DELETE DELETE !!!!!!!!!!!!!!!


#define THREAD_COUNT 2
#define EXECUTION_INTERVAL 30
#define MIN_TASK_TIME 2
#define MAX_TASK_TIME 8

using namespace std;
using namespace chrono;

using read_lock = shared_lock<shared_mutex>;
using write_lock = unique_lock<shared_mutex>;

class task
{

public:
    task() = default;
    task(size_t id, seconds time) : task_id(id), task_time(time) {}
    ~task() = default;

    size_t get_id() const { return task_id; }
    seconds get_time() const { return task_time; }

private:
    size_t task_id;
    seconds task_time;
};

class task_queue
{

public:
    task_queue() = default;
    ~task_queue() = default;

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

    bool pop()
    {
        write_lock _(rw_lock);
        if (tasks.empty())
        {
            return false;
        }

        else
        {
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
};

int main()
{

    return 0;
}