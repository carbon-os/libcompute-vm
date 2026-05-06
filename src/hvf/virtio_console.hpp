#pragma once
#include "hvf/vm_context.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace compute::vm::internal::hvf::virtio {

/// Virtio 1.0 MMIO console device.
///
/// Queue 0 — RX (host → guest): polls rx_fd and feeds into the guest.
/// Queue 1 — TX (guest → host): guest output is written to tx_fd.
class Console {
public:
    Console(VMContext& ctx, uint64_t gpa, int rx_fd, int tx_fd) noexcept;
    ~Console();

    Console(const Console&)            = delete;
    Console& operator=(const Console&) = delete;
    Console(Console&&)                 = delete;
    Console& operator=(Console&&)      = delete;

    [[nodiscard]] uint32_t read (uint32_t off, uint32_t len);
    void                   write(uint32_t off, uint32_t val, uint32_t len);
    void                   notify_irq();

private:
    static constexpr uint32_t kQueueSize = 256;

    struct VDesc {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } __attribute__((packed));

    struct VAvail {
        uint16_t flags, idx;
        uint16_t ring[kQueueSize];
    };

    struct VUsedElem { uint32_t id, len; };

    struct VUsed {
        uint16_t  flags, idx;
        VUsedElem ring[kQueueSize];
    };

    struct Queue {
        uint32_t num        {kQueueSize};
        uint64_t desc_gpa   {0};
        uint64_t avail_gpa  {0};
        uint64_t used_gpa   {0};
        uint16_t last_avail {0};
        bool     ready      {false};
    };

    void process_tx();
    void feed_rx();
    void rx_thread_func();

    VMContext& ctx_;
    std::mutex mtx_;

    int rx_fd_{-1};
    int tx_fd_{-1};

    uint32_t status_       {0};
    uint32_t queue_sel_    {0};
    uint32_t irq_status_   {0};
    uint32_t dev_feat_sel_ {0};
    uint32_t drv_feat_sel_ {0};
    uint64_t features_;

    Queue queues_[2];

    std::atomic<bool> stop_rx_ {false};
    std::thread       rx_thread_;
};

} // namespace compute::vm::internal::hvf::virtio