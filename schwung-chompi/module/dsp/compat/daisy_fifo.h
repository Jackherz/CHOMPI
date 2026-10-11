/**
 * compat/daisy_fifo.h — a libDaisy-compatible FIFO that is safe to share
 * between threads.
 *
 * WHY THIS EXISTS
 * ---------------
 * On the Daisy Seed, CHOMPI's FIFOs cross between interrupt contexts that the
 * firmware serialises with util::ScopedIrqBlocker (see FileStreamingManager.cpp).
 * On Move the same FIFOs are crossed between two *real* threads:
 *
 *     audio callback (SPI, SCHED_FIFO 70, core 3)  <--producer/consumer-->
 *     file I/O worker (SCHED_OTHER, cores 0-2)
 *
 * libDaisy's FIFO is a plain ring of indices; without barriers on a
 * multi-core ARM64 part it is allowed to tear. This class keeps the exact
 * public API CHOMPI uses (so the vendored engine sources stay untouched) but
 * makes the head/tail indices acquire/release atomics, giving a correct
 * single-producer/single-consumer hand-off with no syscalls and no locks.
 *
 * Structural edits (Remove/Insert) are the one case where a producer mutates
 * the middle of the queue. Those take a *try*-lock so they can never block the
 * audio thread: if the consumer is mid-pop the edit is simply skipped, which
 * is the same "request may be dropped" semantic the firmware already has.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace daisy
{

template <typename T, size_t capacity>
class FIFO
{
  public:
    FIFO() = default;

    /** Removes all elements from the FIFO (producer side only). */
    void Clear()
    {
        buffer_out_.store(buffer_in_.load(std::memory_order_acquire), std::memory_order_release);
    }

    size_t MassPushBack(T* buff, size_t size)
    {
        size_t pushed = 0;
        while(pushed < size && PushBack(buff[pushed]))
            pushed++;
        return pushed;
    }

    bool PushBack(const T& elementToAdd)
    {
        const size_t in  = buffer_in_.load(std::memory_order_relaxed);
        const size_t out = buffer_out_.load(std::memory_order_acquire);
        if(GetCount(in, out) == capacity)
            return false;
        buffer_[in] = elementToAdd;
        buffer_in_.store(Next(in), std::memory_order_release);
        return true;
    }

    int PushBack(std::initializer_list<T> valuesToAdd)
    {
        int numPushed = 0;
        for(const T& v : valuesToAdd)
        {
            if(!PushBack(v))
                break;
            numPushed++;
        }
        return numPushed;
    }

    T PopFront()
    {
        const size_t in  = buffer_in_.load(std::memory_order_acquire);
        const size_t out = buffer_out_.load(std::memory_order_relaxed);
        if(in == out)
            return T();
        T val = buffer_[out];
        buffer_out_.store(Next(out), std::memory_order_release);
        return val;
    }

    void PopFrontMany(size_t number)
    {
        for(size_t i = 0; i < number && !IsEmpty(); i++)
            PopFront();
    }

    T& Back()
    {
        const size_t in = buffer_in_.load(std::memory_order_acquire);
        const size_t out = buffer_out_.load(std::memory_order_acquire);
        static T dummy;
        if(in == out)
            return dummy;
        size_t idx = (in == 0) ? kCapacity - 1 : in - 1;
        return buffer_[idx];
    }

    T& Front()
    {
        static T dummy;
        if(IsEmpty())
            return dummy;
        return buffer_[buffer_out_.load(std::memory_order_acquire)];
    }

    T& operator[](size_t idx)
    {
        static T dummy;
        if(idx >= GetNumElements())
            return dummy;
        return buffer_[Next(buffer_out_.load(std::memory_order_acquire), idx)];
    }

    const T& operator[](size_t idx) const
    {
        static T dummy;
        if(idx >= GetNumElements())
            return dummy;
        return buffer_[Next(buffer_out_.load(std::memory_order_acquire), idx)];
    }

    /** Removes the element at idx (producer side, best-effort). */
    bool Remove(size_t idx)
    {
        if(!edit_lock_.test_and_set(std::memory_order_acquire))
        {
            const size_t count = GetNumElements();
            if(idx >= count)
            {
                edit_lock_.clear(std::memory_order_release);
                return false;
            }
            const size_t start = Next(buffer_out_.load(std::memory_order_relaxed), idx);
            for(size_t i = idx; i + 1 < count; i++)
                buffer_[Next(start, i)] = buffer_[Next(start, i + 1)];
            size_t in = buffer_in_.load(std::memory_order_relaxed);
            buffer_in_.store(in == 0 ? kCapacity - 1 : in - 1, std::memory_order_release);
            edit_lock_.clear(std::memory_order_release);
            return true;
        }
        return false; // consumer busy: skip this edit rather than block RT
    }

    bool IsEmpty() const { return buffer_in_.load(std::memory_order_acquire) == buffer_out_.load(std::memory_order_acquire); }

    bool IsFull() const { return GetNumElements() == capacity; }

    size_t GetNumElements() const
    {
        return GetCount(buffer_in_.load(std::memory_order_acquire), buffer_out_.load(std::memory_order_acquire));
    }

    size_t GetCapacity() const { return capacity; }

  private:
    /* libDaisy's ring keeps one slot open so full and empty can be told
     * apart: the array is capacity + 1 long and holds at most `capacity`
     * elements. Getting this wrong wraps a *full* FIFO to "empty" and the
     * samples are silently lost (they were). */
    static constexpr size_t kCapacity = capacity + 1;

    static size_t Next(size_t idx) { return (idx + 1 == kCapacity) ? 0 : idx + 1; }

    static size_t Next(size_t idx, size_t steps) { return (idx + steps) % kCapacity; }

    static size_t GetCount(size_t in, size_t out) { return (in >= out) ? (in - out) : (kCapacity - (out - in)); }

    T buffer_[kCapacity];
    std::atomic<size_t> buffer_in_{0};
    std::atomic<size_t> buffer_out_{0};
    std::atomic_flag edit_lock_ = ATOMIC_FLAG_INIT;
};

} // namespace daisy
