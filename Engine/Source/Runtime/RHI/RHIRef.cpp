module RHI:Ref.impl;

import :Ref;

import std;

namespace SoulEngine {

auto RHIDeferredDeletionQueue::Enqueue(std::function<void()> Destructor) -> void {
    std::scoped_lock Lock(m_Mutex);
    m_Pending.push_back(std::move(Destructor));
}

auto RHIDeferredDeletionQueue::Drain() -> void {
    for (;;) {
        std::vector<std::function<void()>> Batch;
        {
            std::scoped_lock Lock(m_Mutex);
            if (m_Pending.empty())
                return;
            Batch = std::move(m_Pending);
        }
        for (auto& Destroy : Batch)
            Destroy();
    }
}

RHIDeferredDeletionQueue  GDeferredDeletionQueueStorage = {};
RHIDeferredDeletionQueue* GDeferredDeletionQueue        = nullptr;

} // namespace SoulEngine
