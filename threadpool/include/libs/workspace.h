#pragma once

#include <cassert>
#include <future>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <type_traits>
#include <utility>
#include <functional>
#include <iostream>

#include "libs/supervisor.h"
#include "libs/utility.h"
#include "libs/workbranch.h"

namespace sunshine {

// ----------------------------
// 任务类型别名（直接复用 details 命名空间中的类型）
// ----------------------------
namespace task {
using urg = details::urgent;
using nor = details::normal;
using seq = details::sequence;
} // namespace task

// 为外部使用提供便捷别名
using workbranch = details::workbranch;
using supervisor = details::supervisor;
template <typename RT>
using futures = details::futures<RT>;

} // namespace sunshine

namespace sunshine {

/**
 * @class workspace
 * @brief 管理一组 workbranch（工作节点）和 supervisor（监护/管理对象）
 *
 * 设计要点：
 * - workspace 独占拥有 workbranch / supervisor（使用 std::unique_ptr）
 * - 提供 attach/detach 将对象加入/取出（detach 会把所有权返回给调用者）
 * - 使用一个迭代器 cur 做轮询式的负载分配（结合相邻节点的任务数量比较）
 *
 * 注意：
 * - 本类**非线程安全**：若在多线程环境并发调用 attach/detach/submit，需在外部加锁或在此处添加互斥保护。
 * - bid/sid 只是轻量句柄（内部保存裸指针），一旦对应对象被 detach 或 workspace 被销毁，句柄会成为悬空指针。
 */
class workspace {
public:
    // ----------------------------
    // bid: workbranch 句柄（轻量）
    // ----------------------------
    class bid {
        workbranch *ptr = nullptr;
        friend class workspace;

    public:
        explicit bid(workbranch *b) noexcept :
            ptr(b) {
        }

        bool operator==(const bid &other) const noexcept {
            return ptr == other.ptr;
        }
        bool operator!=(const bid &other) const noexcept {
            return ptr != other.ptr;
        }
        bool operator<(const bid &other) const noexcept {
            return ptr < other.ptr;
        }

        friend std::ostream &operator<<(std::ostream &os, const bid &other) {
            os << reinterpret_cast<uint64_t>(other.ptr);
            return os;
        }
    };

    // ----------------------------
    // sid: supervisor 句柄（轻量）
    // ----------------------------
    class sid {
        supervisor *ptr = nullptr;
        friend class workspace;

    public:
        explicit sid(supervisor *s) noexcept :
            ptr(s) {
        }

        bool operator==(const sid &other) const noexcept {
            return ptr == other.ptr;
        }
        bool operator!=(const sid &other) const noexcept {
            return ptr != other.ptr;
        }
        bool operator<(const sid &other) const noexcept {
            return ptr < other.ptr;
        }

        friend std::ostream &operator<<(std::ostream &os, const sid &other) {
            os << reinterpret_cast<uint64_t>(other.ptr);
            return os;
        }
    };

public:
    explicit workspace() = default;

    // 禁用复制/移动语义（管理 unique_ptr）
    workspace(const workspace &) = delete;
    workspace(workspace &&) = delete;

    ~workspace() {
        // 显式清理：虽然容器会自动析构，这里显式清理能在析构日志/调试时更清晰地表达资源释放顺序
        m_branchList.clear();
        m_superMap.clear();
    }

    // ----------------------------
    // attach: 接管一个 workbranch（传入裸指针，workspace 变为所有者）
    // 返回一个轻量句柄（bid）
    // ----------------------------
    bid attach(workbranch *b) {
        assert(b != nullptr);
        m_branchList.emplace_back(b); // 将裸指针封装进 unique_ptr 并放入列表
        cur = m_branchList.begin();   // 重置轮询游标到列表起始（可视为简单策略）
        return bid(b);
    }

    // attach supervisor（同上）
    sid attach(supervisor *s) {
        assert(s != nullptr);
        m_superMap.emplace(s, s); // key 使用裸指针，value 使用 unique_ptr 管理
        return sid(s);
    }

    // ----------------------------
    // detach(bid): 从列表中移除指定 workbranch 并把所有权返回给调用者
    // 使用 move 的方式提取 unique_ptr（比 release + 重建更安全）
    // ----------------------------
    auto detach(bid b) -> std::unique_ptr<workbranch> {
        for (auto it = m_branchList.begin(); it != m_branchList.end(); ++it) {
            if (it->get() == b.ptr) {
                // 先把该 unique_ptr 移出（不制造裸指针）
                auto up = std::move(*it); // up 现在拥有该 workbranch

                // 记录下一个迭代器，用于修正 cur（注意 std::list::erase 不影响其他迭代器）
                auto next_it = std::next(it);

                // 删除容器中的节点
                m_branchList.erase(it);

                // 修正 cur：如果容器现在为空，cur 置为 end()
                // 否则将 cur 指向原来的 next_it（若 next_it==end 则 wrap 到 begin）
                if (m_branchList.empty()) {
                    cur = m_branchList.end();
                } else if (next_it == m_branchList.end()) {
                    cur = m_branchList.begin();
                } else {
                    cur = next_it;
                }

                return up; // 所有权交给调用者
            }
        }
        return nullptr;
    }

    // ----------------------------
    // detach(sid): 从 map 中移除 supervisor 并返回所有权
    // ----------------------------
    auto detach(sid s) -> std::unique_ptr<supervisor> {
        auto it = m_superMap.find(s.ptr);
        if (it == m_superMap.end()) return nullptr;
        auto up = std::move(it->second); // move unique_ptr out
        m_superMap.erase(it);
        return up;
    }

    // ----------------------------
    // for_each: 遍历接口（以引用为参数更通用、安全）
    // ----------------------------
    void for_each(const std::function<void(workbranch &)> &f) {
        for (auto &each : m_branchList) {
            f(*each.get());
        }
    }
    void for_each(const std::function<void(supervisor &)> &f) {
        for (auto &kv : m_superMap) {
            f(*kv.second.get());
        }
    }

    // ----------------------------
    // operator[] / get_ref: 通过句柄快速访问引用
    // 参数采用 const 引用以避免不必要拷贝
    // ----------------------------
    auto operator[](const bid &b) -> workbranch & {
        return *b.ptr;
    }

    auto operator[](const sid &s) -> supervisor & {
        return *s.ptr;
    }

    auto get_ref(const bid &b) -> workbranch & {
        return *b.ptr;
    }

    auto get_ref(const sid &s) -> supervisor & {
        return *s.ptr;
    }

    // ----------------------------
    // submit 模板重载：处理 void 返回、非 void 返回、以及 sequence（seq）任务
    // 这里使用 SFINAE（作为未命名默认模板参数）来区分不同情况
    // 任务调度策略（每次提交都会 advance cur 并比较 cur 与 next 的负载，提交到任务更少的一方）
    // ----------------------------

    // 情况 A: 任务返回 void
    template <typename T = task::nor, typename F,
              typename R = details::result_of_t<F>,
              typename DR = typename std::enable_if<std::is_void<R>::value>::type>
    void submit(F &&task) {
        assert(!m_branchList.empty());
        // this_br 是当前 cur 指向的分支（在调用 forward 之前）
        auto this_br = cur->get();
        // forward(cur) 会把 cur 前进到下一个位置（环回），并返回新的迭代器
        auto next_br = forward(cur)->get();

        // 选择任务量更少的分支来提交（局部负载比较）
        if (next_br->num_tasks() < this_br->num_tasks()) {
            next_br->submit<T>(std::forward<F>(task));
        } else {
            this_br->submit<T>(std::forward<F>(task));
        }
    }

    // 情况 B: 任务有返回值 R（非 void）
    template <typename T = task::nor, typename F,
              typename R = details::result_of_t<F>,
              typename DR = typename std::enable_if<!std::is_void<R>::value>::type>
    auto submit(F &&task) -> std::future<R> {
        assert(!m_branchList.empty());
        auto this_br = cur->get();
        auto next_br = forward(cur)->get();

        if (next_br->num_tasks() < this_br->num_tasks()) {
            return next_br->submit<T>(std::forward<F>(task));
        } else {
            return this_br->submit<T>(std::forward<F>(task));
        }
    }

    // 情况 C: sequence 类型的多任务提交（只在 T == task::seq 时启用）
    template <typename T, typename F, typename... Fs>
    auto submit(F &&f, Fs &&...fs)
        -> typename std::enable_if<std::is_same<T, task::seq>::value>::type {
        assert(!m_branchList.empty());
        auto this_br = cur->get();
        auto next_br = forward(cur)->get();

        if (next_br->num_tasks() < this_br->num_tasks()) {
            next_br->submit<T>(std::forward<F>(f), std::forward<Fs>(fs)...);
        } else {
            this_br->submit<T>(std::forward<F>(f), std::forward<Fs>(fs)...);
        }
    }

private:
    // 别名，便于维护
    using workbranchList = std::list<std::unique_ptr<workbranch>>;
    using supervisorMap = std::map<const supervisor *, std::unique_ptr<supervisor>>;
    using pos_t = workbranchList::iterator;

    // 轮询游标：指向当前选中的 workbranch 的 list 元素
    pos_t cur = {};

    // 实际的容器（unique_ptr 表示 workspace 独占所有权）
    workbranchList m_branchList;
    supervisorMap m_superMap;

private:
    /**
     * @brief forward - 将迭代器前进一位（环回至 begin）
     * @param this_pos - 要前进的迭代器（会被修改）
     * @return 返回修改后的迭代器的引用
     *
     * 注意：调用者必须确保容器非空才调用本函数（submit 中有断言保证）。
     */
    const pos_t &forward(pos_t &this_pos) {
        if (++this_pos == m_branchList.end()) this_pos = m_branchList.begin();
        return this_pos;
    }
};

} // namespace sunshine
