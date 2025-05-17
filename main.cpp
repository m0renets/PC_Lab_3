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
#include <numeric>
#include <algorithm>

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

// Statistic variables
atomic<size_t> created_threads_count = 0;

vector<duration<double>> wait_times;
shared_mutex wait_times_mutex;

vector<size_t> queue_sizes;
mutex queue_stat_mutex;

vector<seconds> execution_times;
mutex execution_times_mtx;

double get_average_wait_time()
{

    lock_guard lock(wait_times_mutex);

    if (wait_times.empty())
    {
        return 0.0;
    }

    duration<double> total_wait_time = accumulate(wait_times.begin(), wait_times.end(), duration<double>(0));
    return total_wait_time.count() / wait_times.size();
}

double get_average_queue_length()
{
    lock_guard lock(queue_stat_mutex);
    if (queue_sizes.empty())
    {
        return 0.0;
    }

    size_t sum = accumulate(queue_sizes.begin(), queue_sizes.end(), 0);
    return static_cast<double>(sum) / queue_sizes.size();
}

double get_average_execution_time()
{
    lock_guard lock(execution_times_mtx);

    if (execution_times.empty())
        return 0.0;

    duration<double> total_wait_time = accumulate(execution_times.begin(), execution_times.end(), duration<double>(0));


    return total_wait_time.count() / execution_times.size();
}

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

        {
            lock_guard lock(queue_stat_mutex);
            queue_sizes.push_back(tasks.size());
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

            {
                lock_guard lock(queue_stat_mutex);
                queue_sizes.push_back(tasks.size());
            }

            return true;
        }
    }

    void emplace(task &&task)
    {
        write_lock _(rw_lock);
        tasks.emplace(move(task));
        
        {
            lock_guard lock(queue_stat_mutex);
            queue_sizes.push_back(tasks.size());
        }
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
        
        {
            lock_guard lock(queue_stat_mutex);
            queue_sizes.push_back(tasks.size());
        }
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
            created_threads_count++;
        }

        initialized = !workers.empty();
    }

    void routine()
    {
        while (true)
        {
            bool task_accquired = false;

            task task;

            auto wait_start = steady_clock::now();

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

            auto wait_end = steady_clock::now();
            duration<double> wait_duration = wait_end - wait_start;

            {
                lock_guard lock(wait_times_mutex);
                wait_times.push_back(wait_duration);
            }

            if (terminated && !task_accquired)
            {
                return;
            }

            seconds exec_time;

            if (task_accquired)
            {
                auto start_time = steady_clock::now();

                task.execute();

                auto end_time = steady_clock::now();
                exec_time = duration_cast<seconds>(end_time - start_time);

            }

            if (terminated)
            {
                return;
            }

            {
                write_lock _(rw_lock);

                if (!execution_paused) // If TP is executing yet
                { 
                    lock_guard lock(cout_mutex);
                    cout << task.get_id() << " task is executed time: " << task.get_time().count() << endl;

                    {
                        lock_guard lock(execution_times_mtx);
                        execution_times.push_back(exec_time);
                    }
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
            if (worker.joinable())
            {
                worker.join();
            }
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

        terminated = false;

        phase_changer = thread(&task_manager::chagePhase, this);
        created_threads_count++;

        generators.reserve(GENERATOR_THREAD_COUNT);
        for (size_t id = 0; id < GENERATOR_THREAD_COUNT; id++)
        {
            generators.emplace_back(&task_manager::generate_and_try_to_insert_task, this);
            created_threads_count++;
        }
    }

    void terminate()
    {

        {
            lock_guard _(tm_mtx);
            terminated = true;
        }

        if (phase_changer.joinable())
        {
            phase_changer.join();
        }

        tp.terminate();

        for (thread &gen : generators)
        {
            if (gen.joinable())
            {
                gen.join();
            }
        }

        generators.clear();
    }

private:
    void chagePhase()
    {
        while (!terminated)
        {

            {
                lock_guard lock(cout_mutex);
                cout << "Phase: " << (phase == poolPhase::Execution ? "Execution" : "Accumulation") << endl;
            }

            this_thread::sleep_for(seconds(EXECUTION_INTERVAL));

            if(terminated)
            {
                return;
            }

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

        while (!terminated)
        {

            seconds waitTime = seconds(distribution(randTime));

            this_thread::sleep_for(seconds(waitTime));

            if (terminated)
            {
                return;
            }

            task task;
            task.task_id = prev_id++;

            {
                lock_guard _(tm_mtx);
                if (phase == poolPhase::Accumulation)
                {
                    {
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

    bool terminated = false;
};

int main()
{

    task_manager tm;

    tm.initialize();

    cin.get();

    cout << "Counts of created threads: " << created_threads_count.load() << endl;

    cout << "Average threads wait time: " << get_average_wait_time() << " seconds" << endl;

    cout << "Average queue length: " << get_average_queue_length() << endl;

    cout << "Average execution time: " << get_average_execution_time() << " seconds" << endl;
    
    tm.terminate();

    return 0;
}