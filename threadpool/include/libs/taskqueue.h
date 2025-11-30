#pragma once
#include <deque>
#include <mutex>

namespace sunshine::details {
template <class T>
class taskQueue {
public:
    using size_type = typename std::deque<T>::size_type;
    void push_back(const T &v) {
        std::lock_guard<std::mutex> lock(tqLock);
        qu.push_back(v);
    }

    void push_back(T &&v) {
        std::lock_guard<std::mutex> lock(tqLock);
        qu.push_back(move(v));
    }

    void push_front(const T &v) {
        std::lock_guard<std::mutex> lock(tqLock);
        qu.push_front(v);
    }

    void push_front(T &&v) {
        std::lock_guard<std::mutex> lock(tqLock);
        qu.push_front(move(v));
    }

    bool try_pop(T &v) {
        std::lock_guard<std::mutex> lock(tqLock);
        if (!qu.empty()) {
            v = std::move(qu.front());
            qu.pop_front();
            return true;
        }
        return false;
    }

    size_type getLength() {
        std::lock_guard<std::mutex> lock(tqLock);
        return qu.size();
    }

private:
    std::mutex tqLock;
    std::deque<T> qu;
};
} // namespace sunshine::details