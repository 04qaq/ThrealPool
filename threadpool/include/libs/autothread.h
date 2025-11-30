#pragma once
#include <thread>

namespace sunshine::details {
struct join {};
struct detach {};
template <typename T>
class autoThread {};

//设计两种类，一种类在包装器销毁时，等待线程结束再销毁，另一种是将线程从前台剥离，在后台运行。
/*
    主要是解决问题：
    在 C++ 标准中，如果一个 std::thread 对象被销毁（例如离开作用域），但它仍然是 joinable 的（即你没有显式调用 join() 或 detach()），
    程序会直接调用 std::terminate() 导致崩溃（Crash）。
*/
template <>
class autoThread<join> {
public:
    using id = std::thread::id;
    autoThread(std::thread &&t) :
        thrd(std::move(t)) {
    }

    autoThread(const std::thread &other) = delete;

    autoThread(autoThread &&other) = default;

    ~autoThread() {
        if (thrd.joinable()) {
            thrd.join(); //等待运行结束
        }
    }
    id getId() {
        return thrd.get_id();
    }

private:
    std::thread thrd;
};

template <>
class autoThread<detach> {
public:
    using id = std::thread::id;
    autoThread(std::thread &&t) :
        thrd(std::move(t)) {
    }

    autoThread(const std::thread &other) = delete;

    autoThread(autoThread &&other) = default;

    ~autoThread() {
        if (thrd.joinable()) {
            thrd.detach(); //线程从主线程剥离
        }
    }
    id getId() {
        return thrd.get_id();
    }

private:
    std::thread thrd;
};
} // namespace sunshine::details