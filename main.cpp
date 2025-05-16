#include <iostream>
#include <chrono>
#include <vector>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <functional>
#include <atomic>
#include <random>
#include <condition_variable>

#include <algorithm> // DELETE DELETE DELETE DELETE
#include <cassert>   // DELETE DELETE DELETE DELETE

#define THREAD_COUNT 2
#define EXECUTION_INTERVAL 30
#define MIN_TASK_TIME 2
#define MAX_TASK_TIME 8
#define GENERATOR_THREAD_COUNT 2
#define GENERATOR_INTERVAL_MIN 2
#define GENERATOR_INTERVAL_MAX 8

using namespace std;
using namespace chrono;

using read_lock = shared_lock<shared_mutex>;
using write_lock = unique_lock<shared_mutex>;

atomic<size_t> prev_id = 0;

shared_mutex sync_mtx;
static mutex cout_mutex;

class task
{

public:
    task() : task_time(random_time()) {};
    task(size_t id, seconds time) : task_id(id), task_time(time) {}
    ~task() = default;

    size_t task_id;

    size_t get_id() const { return task_id; }
    seconds get_time() const { return task_time; }

    void execute()
    {
        this_thread::sleep_for(task_time);

        {
            lock_guard lock(cout_mutex);
            // cout << get_id() << " task is executed time: " << get_time().count() << endl;
        }
    }

private:
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
    ~task_queue() { clear(); };

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

    void push_front(task &&new_task)
    {
        write_lock _(rw_lock);

        queue<task> new_queue;
        new_queue.push(move(new_task));

        while (!tasks.empty())
        {
            new_queue.push(move(tasks.front()));
            tasks.pop();
        }

        tasks.swap(new_queue);
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
        while (true)
        {
            bool task_accquired = false;

            task task;

            {
                write_lock _(rw_lock);
                auto wait_condition = [this, &task_accquired, &task]
                {
                    if (execution_paused)
                    {
                        return false;
                    }

                    task_accquired = tasks.pop(task);
                    return terminated || task_accquired;
                };

                task_waiter.wait(_, wait_condition);
            }

            if (terminated && !task_accquired)
            {
                return;
            }

            if (task_accquired)
            {
                task.execute();
            }

            {
                write_lock _(rw_lock);

                if (!execution_paused)
                { // If TP is executing yet
                    lock_guard lock(cout_mutex);
                    cout << task.get_id() << " task is executed time: " << task.get_time().count() << endl;
                }

                else // didn`t have time to execute
                {
                    tasks.push_front(move(task));
                }
            }
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

        if (!execution_paused)
        {
            task_waiter.notify_one();
        }
    }

    void terminate()
    {
        {
            write_lock _(rw_lock);
            if (working_unsafe())
            {
                terminated = true;
            }

            else
            {
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

    void execution_pause()
    {
        write_lock _(rw_lock);
        if (working_unsafe() && !execution_paused)
        {
            execution_paused = true;
        }
    }

    void execution_resume()
    {
        {
            write_lock _(rw_lock);
            if (execution_paused)
            {
                execution_paused = false;
            }
        }

        task_waiter.notify_all();
    }

private:
    bool initialized = false;
    bool terminated = false;
    bool execution_paused = false;

    task_queue tasks;

    vector<thread> workers;

    mutable shared_mutex rw_lock;
    mutable condition_variable_any task_waiter;
};

class task_manager
{
public:

    task_manager() = default;
    ~task_manager() { terminate(); };

    void initialize()
    {

        phase = poolPhase::Accumulation;
        tp.initialize(THREAD_COUNT);
        tp.execution_pause();

        phase_changer = thread(&task_manager::chagePhase, this);

        generators.reserve(GENERATOR_THREAD_COUNT);
        for (size_t id = 0; id < GENERATOR_THREAD_COUNT; id++)
        {
            generators.emplace_back(&task_manager::generate_and_try_to_insert_task, this);
        }
    }

private:
    void chagePhase()
    {
        while (true)
        {

            {
                lock_guard lock(cout_mutex);
                cout << "Phase: " << (phase == poolPhase::Execution ? "Execution" : "Accumulation") << endl;
            }

            this_thread::sleep_for(seconds(EXECUTION_INTERVAL));

            if (phase == poolPhase::Execution)
            {
                phase = poolPhase::Accumulation;
                tp.execution_pause();
            }

            else
            {
                phase = poolPhase::Execution;
                tp.execution_resume();
            }
        }
    }

    void generate_and_try_to_insert_task()
    {
        static thread_local mt19937 randTime(random_device{}());
        uniform_int_distribution<int> distribution(GENERATOR_INTERVAL_MIN, GENERATOR_INTERVAL_MAX);

        while (true)
        {

            seconds waitTime = seconds(distribution(randTime));

            this_thread::sleep_for(seconds(waitTime));

            task task;
            task.task_id = prev_id++;

            {
                if (phase == poolPhase::Accumulation)
                {
                    {
                        lock_guard _(tm_mtx);
                        tp.add_task(move(task));
                    }

                    {
                        lock_guard lock(cout_mutex);
                        cout << "Add task with id: " << task.get_id() << ", time: " << task.get_time().count() << endl;
                    }
                }
            }
        }
    }

    void terminate()
    {
        {
            lock_guard _(tm_mtx);
            for (thread &gen : generators)
            {
                if (gen.joinable())
                {
                    gen.join();
                }
            }

            generators.clear();
        }

        if (phase_changer.joinable())
        {
            phase_changer.join();
        }
    }

    enum class poolPhase
    {
        Accumulation,
        Execution
    };

    poolPhase phase = poolPhase::Accumulation;

    thread phase_changer;

    vector<thread> generators;

    thread_pool tp;

    mutable shared_mutex tm_mtx;
};

int main()
{

    task_manager tm;

    tm.initialize();

    cin.get();

    return 0;
}