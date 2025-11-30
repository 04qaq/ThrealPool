#pragma once
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

namespace sunshine::details {

// 类型别名，用于兼容不同平台和 C++11/14/17 标准中函数结果类型推导
#if defined(_MSC_VER)
#if _MSVC_LANG >= 201703L
// C++17及以上使用 invoke_result_t
template <typename F, typename... Args>
using result_of_t = std::invoke_result_t<F, Args...>;
#else
// C++11/14 使用 result_of
template <typename F, typename... Args>
using result_of_t = typename std::result_of<F(Args...)>::type;
#endif
#else
#if __cplusplus >= 201703L
// C++17及以上使用 invoke_result_t
template <typename F, typename... Args>
using result_of_t = std::invoke_result_t<F, Args...>;
#else
// C++11/14 使用 result_of
template <typename F, typename... Args>
using result_of_t = typename std::result_of<F(Args...)>::type;
#endif
#endif

// 任务类型标签 (用于函数重载和策略分发)
struct normal {};   // 普通任务
struct urgent {};   // 紧急任务
struct sequence {}; // 串行任务

/**
 * @brief 自定义 function 实现，支持小对象优化 (Small Object Optimization, SOO)
 * @tparam Signature 函数签名 (例如 void(int, float))
 * @tparam InlineSize 内部缓冲区大小，用于避免堆内存分配
 */
template <typename Signature, size_t InlineSize = 64 - sizeof(void *)>
class function_;

// 类型特征：判断 T 是否为 function_ 类型
template <typename T>
struct is_function_ : std::false_type {};

// 偏特化：如果 T 是 function_ 类型，则为 true
template <typename R, size_t N>
struct is_function_<function_<R, N>> : std::true_type {};

// function_ 类的函数签名特化 (R: 返回值类型, Args...: 参数类型包)
template <typename R, typename... Args, size_t InlineSize>
class function_<R(Args...), InlineSize> {
private:
    /**
     * @brief 虚基类：定义了类型擦除的接口
     */
    struct callable_base {
        // 执行函数调用
        virtual R invoke(Args &&...) = 0;
        // 将自身移动到指定缓冲区 (buffer)
        virtual void move_into_(void *buffer) = 0;
        // 将自身拷贝到指定缓冲区 (buffer)
        virtual void clone_into_(void *buffer) const = 0;
        // 虚析构函数，确保通过基类指针可以正确释放资源
        virtual ~callable_base() = default;
    };

    /**
     * @brief 小对象实现：直接存储在 function_ 内部的栈缓冲区中 (SOO)
     * @tparam F 具体的可调用对象类型 (如 lambda, 仿函数)
     */
    template <typename F>
    struct callable_impl : callable_base {
        F f; // 实际存储的用户可调用对象
        // 万能引用 + 完美转发构造，确保高效地接收左值或右值
        template <typename U>
        callable_impl(U &&fn) :
            f(std::forward<U>(fn)) {
        }
        // 实现调用接口，并完美转发参数
        R invoke(Args &&...args) override {
            return f(std::forward<Args>(args)...);
        }

        // 移动操作：在新的缓冲区上使用 Placement New 构造新对象，并从旧对象移动资源
        void move_into_(void *buffer) override {
            // Placement New: 在 buffer 地址上构造 callable_impl，std::move(f) 触发 F 的移动构造
            new (buffer) callable_impl(std::move(f));
        }

        // 拷贝操作：在新的缓冲区上使用 Placement New 构造新对象，并从旧对象拷贝资源
        void clone_into_(void *buffer) const override {
            new (buffer) callable_impl(f);
        }
    };

    // 计算特定类型 F 被 callable_impl 包装后的实际大小
    template <typename T>
    struct callable_size {
        // callable_impl<T> 的大小 = sizeof(callable_base) + sizeof(T)
        static const size_t value = sizeof(callable_impl<T>);
    };

    /**
     * @brief 大对象实现：存储在堆内存中 (当对象大小超过 InlineSize 时使用)
     * @tparam F 具体的可调用对象类型
     */
    template <typename F>
    struct heap_callable_impl : callable_base {
        F *pf; // 指向堆内存中 F 对象的指针

        heap_callable_impl() :
            pf(nullptr){};

        // 构造时在堆上分配内存并构造 F 对象
        template <typename U>
        heap_callable_impl(U &&fn) :
            pf(new F(std::forward<U>(fn))) {
        }

        // 禁用拷贝构造和拷贝赋值，因为堆对象的管理需要显式控制
        heap_callable_impl(const heap_callable_impl &) = delete;
        heap_callable_impl &operator=(const heap_callable_impl &) = delete;

        // 默认的移动构造和移动赋值（只移动指针 pf）
        heap_callable_impl(heap_callable_impl &&) = default;
        heap_callable_impl &operator=(heap_callable_impl &&) = default;

        // 析构函数：释放堆内存
        ~heap_callable_impl() {
            delete pf;
        }

        // 通过指针调用实际函数
        R invoke(Args &&...args) override {
            return (*pf)(std::forward<Args>(args)...);
        }
        // 移动操作：将 pf 指针的所有权转移到新对象
        void move_into_(void *buffer) override {
            auto pc = new (buffer) heap_callable_impl();
            pc->pf = pf;  // 转移指针
            pf = nullptr; // 旧对象指针置空
        }
        // 拷贝操作：深拷贝，在堆上 new 一个新的 F 对象
        void clone_into_(void *buffer) const override {
            // 在 buffer 上构造 heap_callable_impl，并传入 *pf (F的拷贝)
            new (buffer) heap_callable_impl(*pf);
        }
    };

public:
    // 默认构造函数
    function_() = default;

    // nullptr 构造函数
    function_(std::nullptr_t) {
    }

    static constexpr size_t inline_size = InlineSize;

    // 拷贝构造函数
    function_(const function_ &f) {
        if (f.callable) {
            // 调用虚函数 clone_into_，将内容拷贝到自己的 buffer
            f.callable->clone_into_(buffer);
            callable = reinterpret_cast<callable_base *>(&buffer);
        }
    }

    // 移动构造函数
    function_(function_ &&f) noexcept {
        if (f.callable) {
            // 调用虚函数 move_into_，将内容移动到自己的 buffer
            f.callable->move_into_(buffer);
            callable = reinterpret_cast<callable_base *>(&buffer);
            f.callable = nullptr; // 将源对象的指针置空
        }
    }

    // 模板构造函数 A：用于大对象 (大小 > InlineSize)，使用堆实现
    template <typename F, typename T = typename std::decay<F>::type,
              typename std::enable_if<!is_function_<T>::value, int>::type = 0,                // 排除自身类型
              typename std::enable_if<(callable_size<T>::value > InlineSize), int>::type = 0> // 启用：大对象
    function_(F &&f) {
        // 在 buffer 上构造一个管理指针的包装器
        new (buffer) heap_callable_impl<T>(std::forward<F>(f));
        callable = reinterpret_cast<callable_base *>(&buffer);
    }

    // 模板构造函数 B：用于小对象 (大小 <= InlineSize)，使用栈实现 (SOO)
    template <typename F, typename T = typename std::decay<F>::type,
              typename std::enable_if<!is_function_<T>::value, int>::type = 0,                 // 排除自身类型
              typename std::enable_if<(callable_size<T>::value <= InlineSize), int>::type = 0> // 启用：小对象
    function_(F &&f) {
        new (buffer) callable_impl<T>(std::forward<F>(f));
        callable = reinterpret_cast<callable_base *>(&buffer);
    }

    // 拷贝赋值运算符
    function_ &operator=(const function_ &f) {
        if (this != &f) {
            reset(); // 销毁现有内容
            if (f.callable) {
                // 调用 clone_into_
                f.callable->clone_into_(buffer);
                callable = reinterpret_cast<callable_base *>(&buffer);
            }
        }
        return *this;
    }

    // 移动赋值运算符
    function_ &operator=(function_ &&f) noexcept {
        if (this != &f) {
            reset(); // 销毁现有内容
            if (f.callable) {
                f.callable->move_into_(buffer);
                callable = reinterpret_cast<callable_base *>(&buffer);
                f.callable = nullptr; // 将源对象置空
            }
        }
        return *this;
    }

    // 析构函数
    ~function_() {
        reset();
    }

    // 手动调用虚析构函数清理资源
    void reset() {
        if (callable) {
            // 显式调用虚析构函数，正确清理栈或堆上的资源
            callable->~callable_base();
            callable = nullptr;
        }
    }

    // 转换为 bool 类型，判断是否持有可调用对象
    explicit operator bool() const {
        return callable != nullptr;
    }

    // 函数调用操作符
    R operator()(Args... args) const {
        if (!callable)
            throw std::bad_function_call();
        // 通过多态调用 invoke 接口
        return callable->invoke(std::forward<Args>(args)...);
    }

private:
    // 内部存储缓冲区，用于小对象优化。确保最高对齐要求。
    alignas(std::max_align_t) char buffer[InlineSize];
    // 指向 buffer 内构造的 callable_impl 或 heap_callable_impl 对象
    callable_base *callable = nullptr;
};

// 线程池任务的标准类型
using task = function_<void()>;

/**
 * @brief std::future 结果的收集器
 * @tparam T future 的返回类型
 */
template <typename T>
class futures {
private:
    // 使用 deque 存储 future，支持高效的头尾插入，且指针稳定性高
    std::deque<std::future<T>> futs;

public:
    using it = typename std::deque<std::future<T>>::iterator;

    // 等待所有 future 完成
    void wait() {
        for (auto &each : futs) {
            each.wait();
        }
    }
    size_t size() const { // size 函数应为 const
        return futs.size();
    }

    // 获取所有 future 的结果
    std::vector<T> get() {
        std::vector<T> v;
        // future::get() 只能调用一次，且会阻塞直到结果可用
        for (auto &each : futs) {
            v.push_back(each.get());
        }
        return v;
    }

    it end() {
        return futs.end();
    }

    it begin() {
        return futs.begin();
    }

    // 尾部插入 future (使用 emplace 更高效)
    void add_back(std::future<T> &&fut) {
        futs.emplace_back(std::move(fut));
    }
    // 头部插入 future (使用 emplace 更高效)
    void add_front(std::future<T> &&fut) {
        futs.emplace_front(std::move(fut));
    }

    // 遍历所有 future
    void for_each(const std::function<void(std::future<T> &)> &deal) {
        for (auto &each : futs) {
            deal(each);
        }
    }
    // 从 s 迭代器开始遍历
    void for_each(it s, const std::function<void(std::future<T> &)> &deal) {
        for (it current = s; current != futs.end(); ++current) {
            deal(*current);
        }
    }

    // 遍历 [s, e) 范围
    void for_each(it s, it e, const std::function<void(std::future<T> &)> &deal) {
        for (it current = s; current != e; ++current) {
            deal(*current);
        }
    }

    // 重载下标操作符
    auto operator[](size_t idx) -> std::future<T> & {
        return futs[idx];
    }
};
} // namespace sunshine::details