#pragma once
#include <vector>
#include <atomic>
#include <algorithm>

class RingBuffer {
public:
    RingBuffer(size_t capacity = 4096)
        : m_buffer(capacity), m_capacity(capacity), m_writeIndex(0), m_readIndex(0) {}

    void resize(size_t capacity) {
        m_buffer.resize(capacity);
        m_capacity = capacity;
        m_writeIndex.store(0);
        m_readIndex.store(0);
    }

    size_t getCapacity() const { return m_capacity; }

    size_t getAvailableWrite() const {
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        if (writeIdx >= readIdx) {
            return m_capacity - (writeIdx - readIdx) - 1;
        }
        return readIdx - writeIdx - 1;
    }

    size_t getAvailableRead() const {
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        if (writeIdx >= readIdx) {
            return writeIdx - readIdx;
        }
        return m_capacity - (readIdx - writeIdx);
    }

    size_t write(const float* data, size_t count) {
        size_t available = getAvailableWrite();
        size_t toWrite = std::min(count, available);
        if (toWrite == 0) return 0;

        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t firstPart = std::min(toWrite, m_capacity - writeIdx);
        std::copy(data, data + firstPart, m_buffer.begin() + writeIdx);

        if (toWrite > firstPart) {
            size_t secondPart = toWrite - firstPart;
            std::copy(data + firstPart, data + firstPart + secondPart, m_buffer.begin());
            m_writeIndex.store(secondPart, std::memory_order_release);
        } else {
            m_writeIndex.store((writeIdx + firstPart) % m_capacity, std::memory_order_release);
        }

        return toWrite;
    }

    size_t read(float* data, size_t count) {
        size_t available = getAvailableRead();
        size_t toRead = std::min(count, available);
        if (toRead == 0) return 0;

        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        size_t firstPart = std::min(toRead, m_capacity - readIdx);
        std::copy(m_buffer.begin() + readIdx, m_buffer.begin() + readIdx + firstPart, data);

        if (toRead > firstPart) {
            size_t secondPart = toRead - firstPart;
            std::copy(m_buffer.begin(), m_buffer.begin() + secondPart, data + firstPart);
            m_readIndex.store(secondPart, std::memory_order_release);
        } else {
            m_readIndex.store((readIdx + firstPart) % m_capacity, std::memory_order_release);
        }

        return toRead;
    }

    void clear() {
        m_writeIndex.store(0);
        m_readIndex.store(0);
    }

private:
    std::vector<float> m_buffer;
    size_t m_capacity;
    std::atomic<size_t> m_writeIndex;
    std::atomic<size_t> m_readIndex;
};
