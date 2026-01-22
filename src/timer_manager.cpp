
//

#include "timer_manager.h"
#include <algorithm>

void TimerManager::swap_node_(size_t i, size_t j) {
    if (i >= heap_.size() || j >= heap_.size()) return; // 边界保护
    std::swap(heap_[i], heap_[j]);
    ref_[heap_[i].fd] = i; // 同步更新反向索引
    ref_[heap_[j].fd] = j;
}

void TimerManager::siftup_(size_t i) {
    while (i > 0) {
        size_t j = (i - 1) / 2;
        if (heap_[j] > heap_[i]) { // 小根堆逻辑
            swap_node_(i, j);
            i = j;
        } else break;
    }
}

bool TimerManager::siftdown_(size_t index, size_t n) {
    size_t i = index;
    size_t j = i * 2 + 1;
    while (j < n) {
        if (j + 1 < n && heap_[j + 1] < heap_[j]) j++;
        if (heap_[j] < heap_[i]) {
            swap_node_(i, j);
            i = j;
            j = i * 2 + 1;
        } else break;
    }
    return i > index;
}

void TimerManager::AddTimer(int fd, int timeout_ms, const TimeoutCallback& cb) {
    if (ref_.count(fd)) {
        AdjustTimer(fd, timeout_ms); // 如果已存在则更新
        return;
    }
    TimePoint expire = Clock::now() + std::chrono::milliseconds(timeout_ms);
    ref_[fd] = heap_.size();
    heap_.push_back({fd, expire, cb}); // 实例化 TimerNode
    siftup_(heap_.size() - 1);
}

void TimerManager::AdjustTimer(int fd, int timeout_ms) {
    if (ref_.find(fd) == ref_.end()) return; 
    
    size_t i = ref_[fd];
    heap_[i].expire = Clock::now() + std::chrono::milliseconds(timeout_ms);
    if (!siftdown_(i, heap_.size())) {
        siftup_(i);
    }
}

void TimerManager::HandleExpiredTimers() {
    while (!heap_.empty()) {
        TimerNode& node = heap_.front();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(node.expire - Clock::now()).count() > 0) {
            break;
        }
        if (node.cb) node.cb();
        del_(0);
    }
}

void TimerManager::del_(size_t index) {
    if (heap_.empty() || index >= heap_.size()) return;
    
    size_t n = heap_.size() - 1;
    if (index < n) {
        swap_node_(index, n);
        if (!siftdown_(index, n)) siftup_(index);
    }
    ref_.erase(heap_.back().fd); // 统一使用 fd
    heap_.pop_back();
}

int TimerManager::GetNextTimeout() {
    HandleExpiredTimers();
    if (heap_.empty()) return -1;
    auto res = std::chrono::duration_cast<std::chrono::milliseconds>(heap_.front().expire - Clock::now()).count();
    return res < 0 ? 0 : static_cast<int>(res);
}

void TimerManager::RemoveTimer(int fd) {
    if (ref_.count(fd)) {
        del_(ref_[fd]);
    }
}