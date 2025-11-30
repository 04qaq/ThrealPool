#pragma once
// workbranch.hpp
// 修正版：按照模板实现的线程工作分支（包含详细中文注释）

#include <condition_variable>
#include <cstdlib>
#include <future>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <chrono>
#include <exception>
#include <libs/autothread.h>
#include <libs/taskqueue.h>
#include <libs/utility.h>

namespace sunshine {

/// 等待策略（与模板语义一致）
enum class waitStrategy {
    lowlatancy, // busy-wait + yield：最低延迟，CPU 占用高
    balance,    // 前一段 busy-wait，达到阈值后短暂 sleep：折中
    blocking    // 使用条件变量阻塞，CPU 占用低但延迟较高
};

namespace details {

// 任务类型（工作线程执行的基本单元）
using task_t = std::function<void()>;

// 注意：下面的 worker / taskqueue 类型名请与工程实际一致。
// 假设 autothread<detach> 提供类型 member id，可以用作 map 的 key。
class workbranch {
public:
    using worker = autoThread<detach>;
    using worker_map = std::map<worker::id, worker>;

    /**
     * @brief 构造函数：创建 wks 个 worker（至少 1 个），设置等待策略
     * @param wks 初始 worker 数量（最少 1）
     * @param strategy 等待策略
     */
    explicit workbranch(int wks = 1, waitStrategy strategy = waitStrategy::lowlatancy) {
        wait_strategy = strategy;
        // 保证至少创建 1 个 worker
        for (int i = 0; i < std::max(wks, 1); ++i) {
            add_worker();
        }
    }

    // 禁止拷贝/移动（内部持有线程、互斥量等不可安全复制）
    workbranch(const workbranch &) = delete;
    workbranch(workbranch &&) = delete;

    /**
     * @brief 析构：发出退出请求并等待所有 worker 安全退出
     *
     * 实现思路：
     *  - 将 decline 设为当前 worker 数量（每个 worker 会处理一个退出请求）
     *  - 标记 destructing = true（用于唤醒阻塞策略下的线程）
     *  - 对于 blocking 策略，notify_all 唤醒可能阻塞的线程
     *  - 在 thread_cv 上等待 decline 被减为 0（表示所有退出请求已被处理）
     */
    ~workbranch() {
        std::unique_lock<std::mutex> lock(lok);
        decline = workers.size();
        destructing = true;
        if (wait_strategy == waitStrategy::blocking) task_cv.notify_all();
        // 等待直到 decline 被 worker 自行递减为 0
        thread_cv.wait(lock, [this] { return !decline; });
    }

public:
    /**
     * @brief 添加一个 worker（线程）
     * @note O(log N) 因为向 map 插入
     */
    void add_worker() {
        std::lock_guard<std::mutex> lock(lok);
        std::thread t(&workbranch::mission, this);
        workers.emplace(t.get_id(), std::move(t)); // 将线程对象放入 map（key 为 id）
    }

    /**
     * @brief 删除一个 worker（请求一个线程退出）
     * 设计：这里不是强制终止某个线程，而是将退出请求计数（decline++），
     * 当某个 worker 看到 decline>0 时，会自行退出并从 workers 中移除自己。
     */
    void del_worker() {
        std::lock_guard<std::mutex> lock(lok);
        if (workers.empty()) {
            throw std::runtime_error("workbranch: No worker in workbranch to delete");
        } else {
            // 请求减少一个 worker（由某个线程在安全点响应）
            decline++;
            // 如果使用阻塞策略，唤醒一个阻塞的 worker 以便它能尽快看到 decline
            if (wait_strategy == waitStrategy::blocking) task_cv.notify_one();
        }
    }

    /**
     * @brief 等待当前所有任务完成并暂停 worker（减轻系统压力）
     * @param timeout 毫秒，默认 unsigned(-1) 表示无限等待
     * @return true 如果在 timeout 内完成等待（否则 false）
     *
     * 协议（两阶段）：
     *  1) is_waiting = true；唤醒阻塞 worker（如果是 blocking 策略）；
     *     等待 task_done_workers >= workers.size()，表示所有 worker 都已报告自己空闲
     *  2) is_waiting = false；thread_cv.notify_all() 恢复 worker；
     *     再等待所有 worker 报告已从等待恢复（waiting_finished_worker >= workers.size()）
     */
    bool wait_tasks(unsigned timeout = static_cast<unsigned>(-1)) {
        bool res;
        {
            std::unique_lock<std::mutex> locker(lok);
            m_is_waiting = true; // worker 将上报空闲
            if (wait_strategy == waitStrategy::blocking) task_cv.notify_all();
            // 等待所有 worker 报告空闲（或超时）
            res = task_done_cv.wait_for(locker, std::chrono::milliseconds(timeout), [this] {
                return task_done_workers >= workers.size();
            });
            // 清理计数并关闭等待标志（先关闭再发 recover 信号）
            task_done_workers = 0;
            m_is_waiting = false;
        }
        // 告知所有 worker 恢复
        thread_cv.notify_all();

        // 再等待所有 worker 报告已从等待中恢复
        std::unique_lock<std::mutex> locker(lok);
        waiting_finished.wait(locker, [this] { return waiting_finished_worker >= workers.size(); });
        waiting_finished_worker = 0;
        return res;
    }

    /**
     * @brief 返回 worker 数量（线程安全）
     */
    size_t num_workers() {
        std::lock_guard<std::mutex> lock(lok);
        return workers.size();
    }

    /**
     * @brief 返回任务队列中的任务数（依赖 taskqueue::length() 线程安全）
     */
    size_t num_tasks() {
        return tq.getLength();
    }

public:
    // ------------------ submit（普通 void 任务） ------------------
    template <typename T = normal, typename F, typename R = result_of_t<F>,
              typename DR = typename std::enable_if<std::is_void<R>::value>::type>
    auto submit(F &&task) -> typename std::enable_if<std::is_same<T, normal>::value>::type {
        // 把可调用对象包装为 std::function<void()>
        std::function<void()> fn = std::forward<F>(task);
        tq.push_back([fn]() mutable {
            try {
                fn();
            } catch (const std::exception &ex) {
                std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                          << "] caught exception:\n  what(): " << ex.what() << '\n'
                          << std::flush;
            } catch (...) {
                std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                          << "] caught unknown exception\n"
                          << std::flush;
            }
        });
        if (wait_strategy == waitStrategy::blocking) task_cv.notify_one();
    }

    // ------------------ submit（紧急 void 任务，插队执行） ------------------
    template <typename T, typename F, typename R = result_of_t<F>,
              typename DR = typename std::enable_if<std::is_void<R>::value>::type>
    auto submit(F &&task) -> typename std::enable_if<std::is_same<T, urgent>::value>::type {
        std::function<void()> fn = std::forward<F>(task);
        tq.push_front([fn]() mutable {
            try {
                fn();
            } catch (const std::exception &ex) {
                std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                          << "] caught exception:\n  what(): " << ex.what() << '\n'
                          << std::flush;
            } catch (...) {
                std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                          << "] caught unknown exception\n"
                          << std::flush;
            }
        });
        if (wait_strategy == waitStrategy::blocking) task_cv.notify_one();
    }

    // ------------------ submit（sequence：把多个可调用对象合并成一个任务按序执行） ------------------
    template <typename T, typename F, typename... Fs>
    auto submit(F &&task, Fs &&...tasks) -> typename std::enable_if<std::is_same<T, sequence>::value>::type {
        // 用值捕获保证闭包中对象的生命周期
        auto bound = std::make_shared<std::tuple<std::decay_t<F>, std::decay_t<Fs>...>>(
            std::forward<F>(task), std::forward<Fs>(tasks)...);
        tq.push_back([bound, this]() {
            try {
                // 通过 rexec 展开并按序执行。这里直接使用捕获的 tuple 里存的函数对象。
                // 为简单起见，使用 lambda 调用 rexec（rexec 本身使用模板展开）
                // 我们需要将 tuple 中元素展开为参数 —— 这里用 helper
                apply_sequence_and_rexec(*bound);
            } catch (const std::exception &ex) {
                std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                          << "] caught exception:\n  what(): " << ex.what() << '\n'
                          << std::flush;
            } catch (...) {
                std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                          << "] caught unknown exception\n"
                          << std::flush;
            }
        });
        if (wait_strategy == waitStrategy::blocking) task_cv.notify_one();
    }

    // ------------------ submit（普通返回值任务，返回 future） ------------------
    template <typename T = normal, typename F, typename R = result_of_t<F>,
              typename DR = typename std::enable_if<!std::is_void<R>::value, R>::type>
    auto submit(F &&task, typename std::enable_if<std::is_same<T, normal>::value, normal>::type = {})
        -> std::future<R> {
        // 使用 std::function<R()> 包装可调用对象并用 shared_ptr 管理 promise 保证生命周期
        std::function<R()> exec = std::forward<F>(task);
        auto task_promise = std::make_shared<std::promise<R>>();
        tq.push_back([exec = std::move(exec), task_promise]() {
            try {
                task_promise->set_value(exec());
            } catch (...) {
                try {
                    task_promise->set_exception(std::current_exception());
                } catch (const std::exception &ex) {
                    std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                              << "] caught exception while setting promise:\n  what(): " << ex.what() << '\n'
                              << std::flush;
                } catch (...) {
                    std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                              << "] caught unknown exception while setting promise\n"
                              << std::flush;
                }
            }
        });
        if (wait_strategy == waitStrategy::blocking) task_cv.notify_one();
        return task_promise->get_future();
    }

    // ------------------ submit（紧急返回值任务，插队执行并返回 future） ------------------
    template <typename T, typename F, typename R = result_of_t<F>,
              typename DR = typename std::enable_if<!std::is_void<R>::value, R>::type>
    auto submit(F &&task, typename std::enable_if<std::is_same<T, urgent>::value, urgent>::type = {})
        -> std::future<R> {
        std::function<R()> exec = std::forward<F>(task);
        auto task_promise = std::make_shared<std::promise<R>>();
        tq.push_front([exec = std::move(exec), task_promise]() {
            try {
                task_promise->set_value(exec());
            } catch (...) {
                try {
                    task_promise->set_exception(std::current_exception());
                } catch (const std::exception &ex) {
                    std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                              << "] caught exception while setting promise:\n  what(): " << ex.what() << '\n'
                              << std::flush;
                } catch (...) {
                    std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                              << "] caught unknown exception while setting promise\n"
                              << std::flush;
                }
            }
        });
        if (wait_strategy == waitStrategy::blocking) task_cv.notify_one();
        return task_promise->get_future();
    }

private:
    // helper: 将 tuple 中的函数按序展开并交给 rexec 执行
    // 这里使用 index_sequence 展开 tuple 的元素并调用 rexec
    template <typename Tup, std::size_t... I>
    void apply_tuple_to_rexec_impl(Tup &tup, std::index_sequence<I...>) {
        // 将 tuple 元素按参数传递给 rexec
        rexec(std::get<I>(tup)...);
    }

    template <typename Tup>
    void apply_sequence_and_rexec(Tup &tup) {
        constexpr std::size_t N = std::tuple_size<Tup>::value;
        apply_tuple_to_rexec_impl(tup, std::make_index_sequence<N>{});
    }

private:
    // 主循环（worker 运行体），在单独线程中执行
    void mission() {
        task_t task;
        int spin_count = 0;

        while (true) {
            // 优先：当没有退出请求且队列有任务时，立刻取并执行任务
            if (decline <= 0 && tq.try_pop(task)) {
                try {
                    task();
                } catch (...) {
                    // 一般不应到这里，因为任务包装中已捕获异常，但以防万一保底捕获
                    std::cerr << "workbranch: worker[" << std::this_thread::get_id()
                              << "] unexpected exception in task\n"
                              << std::flush;
                }
                spin_count = 0;
            }
            // 有退出请求（del_worker 或 析构时设置的 decline）
            else if (decline > 0) {
                std::lock_guard<std::mutex> lock(lok);
                // double-check：在加锁后再次检测并递减 decline
                if (decline > 0 && decline--) {
                    // 从 workers 容器中移除自身（key 为当前线程 id）
                    workers.erase(std::this_thread::get_id());
                    // 如果当前处于 wait_tasks 的 is_waiting 阶段，需上报 task_done
                    if (m_is_waiting) task_done_cv.notify_one();
                    // 如果正在析构，通知析构等待者（~workbranch）
                    if (destructing) thread_cv.notify_one();
                    // 线程退出（mission 返回）
                    return;
                }
            }
            // 没有任务也没有退出请求
            else {
                if (m_is_waiting) {
                    // wait_tasks 协商的第一阶段：上报自己已空闲并阻塞等待恢复
                    std::unique_lock<std::mutex> locker(lok);
                    task_done_workers++;
                    task_done_cv.notify_one(); // 告知等待者（wait_tasks）已有一个 worker 报告空闲
                    // 阻塞直到 is_waiting 变为 false（由 wait_tasks 恢复）
                    thread_cv.wait(locker, [this] { return !m_is_waiting; });
                    // 恢复后上报已恢复
                    waiting_finished_worker++;
                    if (waiting_finished_worker >= workers.size()) waiting_finished.notify_one();
                } else {
                    // 根据等待策略采取相应动作
                    switch (wait_strategy) {
                    case waitStrategy::lowlatancy: {
                        std::this_thread::yield();
                        break;
                    }
                    case waitStrategy::balance: {
                        if (spin_count < max_spin_count) {
                            ++spin_count;
                            std::this_thread::yield();
                        } else {
                            // 短暂 sleep，降低 CPU 占用
                            std::this_thread::sleep_for(std::chrono::nanoseconds(1));
                        }
                        break;
                    }
                    case waitStrategy::blocking: {
                        std::unique_lock<std::mutex> locker(lok);
                        // 阻塞直到有任务、或被请求等待、或析构/退出请求
                        task_cv.wait(locker, [this] {
                            return tq.getLength() > 0 || m_is_waiting || destructing || decline > 0;
                        });
                        break;
                    }
                    } // switch
                }     // if m_is_waiting
            }         // outer if
        }             // while
    }

    // 递归顺序执行辅助（sequence 提交使用）
    template <typename F>
    void rexec(F &&func) {
        func();
    }

    template <typename F, typename... Fs>
    void rexec(F &&func, Fs &&...funcs) {
        func();
        rexec(std::forward<Fs>(funcs)...);
    }

private:
    const int max_spin_count = 10000; // balance 策略忙等上限（可调）

    // 工作线程容器与任务队列
    worker_map workers = {};
    taskQueue<task_t> tq = {};

    // 策略与协商/状态
    waitStrategy wait_strategy = {};
    size_t decline = 0;                 // 希望退出的线程数量（del_worker 或 析构时设置）
    size_t task_done_workers = 0;       // wait_tasks 阶段：上报空闲的 worker 数
    size_t waiting_finished_worker = 0; // wait_tasks 恢复阶段：已恢复的 worker 数
    bool m_is_waiting = false;          // 是否正在进行 wait_tasks 的等待阶段
    bool destructing = false;           // 析构中标志

    // 同步原语
    std::mutex lok;
    std::condition_variable thread_cv;        // 用于析构/恢复唤醒
    std::condition_variable task_done_cv;     // wait_tasks 等待空闲 worker 的计数唤醒
    std::condition_variable task_cv;          // blocking 策略下用于唤醒有任务的 worker
    std::condition_variable waiting_finished; // wait_tasks 等待恢复完成
};

} // namespace details
} // namespace sunshine
