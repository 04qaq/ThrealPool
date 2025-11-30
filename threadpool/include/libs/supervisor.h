#pragma once

#include "libs/autothread.h"
#include "libs/workbranch.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm> // for std::min

namespace sunshine {
namespace details {

// 自动扩缩容（Auto-scaling）管理器
// 自动根据工作量的大小，增加或减少线程数，达到负载均衡
class supervisor {
public:
    // 类型别名定义
    using tickCallbackT = std::function<void()>;
    using workBranchPtr = std::shared_ptr<workbranch>;

private:
    // 成员变量
    bool m_stopping = false;   // 停止标志位
    size_t m_wmin = 0;         // 最小工人数
    size_t m_wmax = 0;         // 最大工人数
    unsigned int m_tout = 0;   // 当前超时时间 (毫秒)
    const unsigned int m_tval; // 默认超时时间 (恢复用)

    std::mutex m_SupLock;             // 互斥锁
    std::condition_variable m_thrdCv; // 条件变量

    tickCallbackT m_tickCb;                // 周期回调函数
    std::vector<workBranchPtr> m_branches; // 受监管的分支列表 (修正拼写: branchs -> branches)

    // 注意：worker 的初始化放在最后，确保所有成员变量就绪后再启动线程
    autoThread<join> m_worker;

public:
    /**
     * @brief 构造函数
     * @param wmin 最小线程数
     * @param wmax 最大线程数
     * @param tout 超时时间/检查间隔 (ms)
     * @param tickCb 每次检查后的回调
     */
    explicit supervisor(
        size_t wmin, size_t wmax, unsigned int tout = 500, tickCallbackT tickCb = [] {}) :
        m_wmin(wmin),
        m_wmax(wmax), m_tout(tout), m_tval(tout), m_tickCb(tickCb), m_worker(std::thread(&supervisor::mission, this)) { // 启动后台监视线程

        // 确保参数逻辑正确：最大值必须大于最小值，且大于0
        assert(wmin >= 0 && wmax > 0 && wmax > wmin);
    }

    // 禁止拷贝和移动，保证唯一性
    supervisor(const supervisor &) = delete;
    supervisor(supervisor &&) = delete;

    // 析构函数
    ~supervisor() {
        {
            std::lock_guard<std::mutex> lock(m_SupLock);
            m_stopping = true;     // 设置停止标志
            m_thrdCv.notify_one(); // 【关键】唤醒可能正在休眠的线程，使其能检查到 stopping 状态
        }
        // m_worker (autothread) 析构时会自动 join 等待线程结束
    }

public:
    /**
     * @brief 添加一个需要监管的工作分支
     */
    void add_super(workBranchPtr &b) {
        std::lock_guard<std::mutex> lock(m_SupLock);
        m_branches.push_back(b);
    }

    /**
     * @brief 挂起监视器 (暂停工作)
     * @param t 暂停的时长，默认为最大无符号整数 (相当于无限长)
     */
    void suspend(unsigned int t = -1) {
        std::lock_guard<std::mutex> lock(m_SupLock);
        m_tout = t;
    }

    /**
     * @brief 恢复监视器工作
     */
    void proceed() {
        {
            std::lock_guard<std::mutex> lock(m_SupLock);
            m_tout = m_tval; // 恢复默认时间间隔
        }
        m_thrdCv.notify_one();
    }

    /**
     * @brief 设置周期回调
     */
    void setCb(tickCallbackT cb) {
        std::lock_guard<std::mutex> lock(m_SupLock);
        m_tickCb = cb;
    }

private:
    /**
     * @brief 后台任务主循环
     */
    void mission() {
        while (!m_stopping) {
            try {
                {
                    std::unique_lock<std::mutex> lock(m_SupLock);

                    // 遍历所有受监管的分支
                    for (auto ptr : m_branches) {
                        // 假设 workbranch 类有 num_workers() 接口
                        size_t workNums = ptr->num_workers();
                        size_t taskNums = ptr->num_tasks();

                        // 策略：扩容 (Scale Up)
                        if (taskNums > 0) { // 如果有积压任务
                            assert(workNums <= m_wmax);
                            // 计算需要增加的人手：
                            // 既不能超过最大工人数限制 (m_wmax - workNums)
                            // 也不需要超过积压的任务数 (taskNums - workNums)
                            // 注意：这里需要防止无符号数减法溢出，通常 num_tasks() > num_workers() 才进这里
                            // 但为了安全，直接计算缺口
                            size_t needed = (taskNums > workNums) ? (taskNums - workNums) : 0;
                            size_t capacity = m_wmax - workNums;

                            size_t nums_to_add = std::min(capacity, needed);

                            for (size_t i = 0; i < nums_to_add; i++) {
                                ptr->add_worker(); // 快速增加
                            }
                        }
                        // 策略：缩容 (Scale Down)
                        else if (workNums > m_wmin) {
                            ptr->del_worker(); // 慢速减少 (每次循环减一个)
                        }
                    }

                    // 等待：释放锁并休眠 m_tout 毫秒
                    // wait_for 返回 false 表示超时（正常周期），返回 true 表示被 notify 唤醒（可能析构或恢复）
                    if (!m_stopping) {
                        m_thrdCv.wait_for(lock, std::chrono::milliseconds(m_tout));
                    }
                }

                // 执行周期回调 (在锁外执行，防止阻塞其他操作)
                if (m_tickCb) m_tickCb();

            } catch (const std::exception &e) {
                // 异常捕获，防止线程崩溃
                std::cerr << "workspace: supervisor[" << std::this_thread::get_id() << "] caught exception:\n"
                          << "what(): " << e.what() << "\n"
                          << std::flush;
            }
        }
    }
};

}
} // namespace sunshine::details