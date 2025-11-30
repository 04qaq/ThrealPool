
# ThreadPool
主要是对github项目workspace的学习与模仿，特别感谢作者项目的教导

> 轻量、可扩缩的 C++17 线程/任务调度框架。包含：
> * `autoThread`（RAII 封装的 `std::thread`，析构时 join/detach）
> * 线程安全的 `taskQueue`（双端队列）
> * `workbranch`（每个分支独立线程池，支持多等待策略、普通/紧急/序列和有返回值任务）
> * `supervisor`（后台自动扩缩容模块）
> * `workspace`（多分支管理、负载感知的轮询调度）
> * 自定义高效 `function_`（含 Small-Object Optimization）与 `futures` 收集器

---

## 目录

* [项目目标](#项目目标)
* [关键特性](#关键特性)
* [设计概览](#设计概览)
* [快速开始](#快速开始)

  * [构建（CMake）](#构建cmake)
  * [最小使用示例](#最小使用示例)
* [核心 API 说明（示例代码）](#核心-api-说明示例代码)

  * [`autoThread`](#autothread)
  * [`taskQueue<T>`](#taskqueuet)
  * [`workbranch`](#workbranch)
  * [`supervisor`](#supervisor)
  * [`workspace`](#workspace)
* [调优建议](#调优建议)
* [常见问题（FAQ）](#常见问题faq)
* [贡献 & 许可证](#贡献--许可证)

---

## 项目目标

本项目旨在为需要高并发任务调度与弹性线程管理的服务端或中间件提供一个简单、可扩展且高性能的基础设施。它适合作为协程调度器、网络服务器或异步任务框架的底层执行层。

---

## 关键特性

* RAII 风格线程封装：析构自动 `join()` 或 `detach()`，避免 `std::terminate()`。
* 线程安全的双端任务队列，支持前插（urgent）与尾插（normal）。
* `workbranch` 支持三种等待策略：`lowlatency`、`balance`、`blocking`，在延迟与 CPU 使用之间权衡。
* 支持普通任务、有返回值任务（`std::future`）、紧急任务（插队）和序列任务（按序执行多任务）。
* `supervisor` 后台自动扩缩容，根据任务积压自动 `add_worker` / `del_worker`。
* `workspace` 实现 round-robin + 本地队列长度比较的轻量负载均衡。
* 自定义 `function_` 支持小对象优化，降低堆分配。

---

## 设计概览

* **workbranch**：每个分支维护一组 worker（线程），以及一个 `taskQueue`。线程执行主循环 `mission()`，从队列取任务或基于等待策略休眠/忙等。扩缩容通过 `add_worker()` / `del_worker()` 触发。
* **supervisor**：运行在独立线程里，周期性遍历受管 `workbranch`，根据 `num_tasks()` 与 `num_workers()` 差异进行弹性调整。
* **workspace**：上层管理器，持有多个 `workbranch`（用 `std::list<std::unique_ptr<workbranch>>`），并提供 `submit` 接口将任务路由到合适分支（结合轮询与局部负载比较）。
* **函数封装**：`function_` 采用 SOO（Small Object Optimization）以减少短小任务的堆分配。

---

## 快速开始

### 构建（CMake）

示例 `CMakeLists.txt`（骨架）：

```cmake
cmake_minimum_required(VERSION 3.10)
project(sunshine_threadpool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra -pthread")

add_library(sunshine
    libs/autothread.h
    libs/taskqueue.h
    libs/workbranch.h
    libs/supervisor.h
    libs/function_.h
    libs/workspace.h
    # ... 其它源文件
)

add_executable(example examples/example.cpp)
target_link_libraries(example PRIVATE sunshine)
```

构建命令：

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

---

## 最小使用示例

这是一个简单示范：创建一个 `workbranch`，提交几个任务，启动 `supervisor` 监控，然后用 `workspace` 做多分支调度。

```cpp
#include "libs/workbranch.h"
#include "libs/supervisor.h"
#include "libs/workspace.h"

using namespace sunshine::details;
using namespace sunshine;

int main() {
    // 单分支示例
    workbranch wb(2, waitStrategy::blocking); // 2 workers, blocking wait
    // 提交简单任务
    wb.submit<>([](){ printf("hello from task\n"); });

    // supervisor 示例（最小）
    auto sp = std::make_unique<supervisor>(1, 8, 300); // min=1, max=8, tout=300ms
    sp->add_super(std::shared_ptr<workbranch>(&wb, [](workbranch*){})); // 仅示例，注意生命周期管理

    // workspace 示例
    workspace ws;
    ws.attach(new details::workbranch(2));
    ws.submit<>([](){ puts("submitted to workspace"); });

    return 0;
}
```

> 注意：示例中对 `add_super` 的传参仅为展示，实际请传入 `shared_ptr<workbranch>` 并做好生命周期管理（不要把栈对象传给 shared_ptr 的裸删除器）。

---

## 核心 API 说明（示例代码）

### `autoThread`

RAII 封装 `std::thread`：

* `autoThread<join>`：析构时 `join()`（等待线程结束）。
* `autoThread<detach>`：析构时 `detach()`（线程后台运行）。

示例：

```cpp
autoThread<join> worker(std::thread([]{
    // do work
}));
```

### `taskQueue<T>`

线程安全双端队列，主要接口：

* `push_back(const T&)`, `push_front(T&&)`, `try_pop(T &out)`, `getLength()`。

示例：

```cpp
taskQueue<std::function<void()>> tq;
tq.push_back([]{ printf("task\n"); });
std::function<void()> fn;
if (tq.try_pop(fn)) fn();
```

### `workbranch`

重要接口：

* 构造：`workbranch(int initial_workers = 1, waitStrategy strat = waitStrategy::lowlatancy);`
* `add_worker()`, `del_worker()`
* `submit<T>(callable...)`：模板支持 `normal/urgent/sequence` 与有无返回值版本
* `num_workers()`, `num_tasks()`
* `wait_tasks(unsigned timeout_ms = -1)`

示例（提交带返回值任务）：

```cpp
auto fut = wb.submit<details::normal>([]()->int {
    return 42;
});
int r = fut.get(); // 42
```

### `supervisor`

构造：

```cpp
supervisor(size_t wmin, size_t wmax, unsigned int tout = 500, tickCallbackT tickCb = []{})
```

功能：

* `add_super(const std::shared_ptr<workbranch>& b)`
* `suspend(unsigned int t)`, `proceed()`, `setCb(tickCallbackT)`

`supervisor` 在构造时启动后台线程，析构时会通知线程停止并等待退出（通过 `autoThread<join>`）。

### `workspace`

管理多个 `workbranch`，接口：

* `bid attach(workbranch* b)`：接管裸指针（转为 `unique_ptr`）并返回句柄
* `std::unique_ptr<workbranch> detach(bid id)`：移除并返还所有权
* `submit<F>`：自动选分支并提交（多重模板支持 void/return/sequence）
* `for_each(...)`, `operator[](bid)` 等

示例（多分支任务提交）：

```cpp
workspace ws;
auto id1 = ws.attach(new details::workbranch(2));
auto id2 = ws.attach(new details::workbranch(2));

ws.submit<>([](){ puts("task1"); });
auto fut = ws.submit<>([]()->int{ return 7; });
int v = fut.get();
```

> 注意：`bid/sid` 是轻量裸指针句柄，使用时请确保对象尚未被 `detach` 或销毁。

---

## 调优建议

1. **等待策略**

   * `lowlatency`：延迟最低但 CPU 占用高，适合极低延迟场景。
   * `balance`：忙等 + 短时 sleep，适合中等负载。
   * `blocking`：使用条件变量，CPU 占用低但延迟略高，适合任务间隔明显的场景。

2. **worker 数量**

   * 初始 worker 可设置为 `std::thread::hardware_concurrency()` 或更少（依据任务 IO/CPU 特性调整）。
   * 通过 `supervisor` 上下界（`wmin`/`wmax`）控制扩缩容范围以避免过度创建线程。

3. **任务粒度**

   * 尽量减少超短任务（高频小任务会引发队列/调度开销），可考虑合批提交或使用 sequence 合并。

4. **避免数据竞争**

   * 框架里部分变量（如 `decline`）对并发访问要小心。如果要在高并发中大量动态增减 worker，请启用 TSAN 进行验证，或将部分状态改为 `std::atomic`。

---

## 常见问题（FAQ）

**Q1：我可以把 `workbranch` 存成栈对象再传给 `supervisor` 吗？**
A：不推荐。`supervisor` 存放 `shared_ptr<workbranch>`，请保证 `workbranch` 的生命周期与 `shared_ptr` 一致。若传入栈对象，必须确保不会在 `supervisor` 使用期间析构。

**Q2：如何安全地删除某个 worker？**
A：调用 `del_worker()` 会增加退出请求计数，某个 worker 在安全点看到该请求后会自己退出并从容器中移除，避免强制终止线程。

**Q3：如何诊断任务未执行或死锁？**
A：

* 检查是否使用了错误的等待策略（blocking 情况下忘记 `task_cv.notify_*()`）；
* 使用日志或断点观察 `tq.getLength()` 与 `num_workers()`；
* 启用 ThreadSanitizer（TSAN）检测数据竞争。

