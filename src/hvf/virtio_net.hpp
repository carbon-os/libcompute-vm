#pragma once
#include "hvf/vm_context.hpp"
#include "hvf/network.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace compute::vm::internal::hvf::virtio {

class Net {
public:
    Net(VMContext& ctx, uint64_t gpa, const NetDeviceConfig& cfg) noexcept;
    ~Net();

    Net(const Net&)            = delete;
    Net& operator=(const Net&) = delete;
    Net(Net&&)                 = delete;
    Net& operator=(Net&&)      = delete;

    [[nodiscard]] uint32_t read (uint32_t off, uint32_t len);
    void                   write(uint32_t off, uint32_t val, uint32_t len);
    void                   notify_irq();

private:
    static constexpr uint32_t kQueueSize = 256;

    struct VirtioNetHdr {
        uint8_t  flags, gso_type;
        uint16_t hdr_len, gso_size, csum_start, csum_offset, num_buffers;
    } __attribute__((packed));

    struct VDesc { uint64_t addr; uint32_t len; uint16_t flags, next; } __attribute__((packed));
    struct VAvail { uint16_t flags, idx; uint16_t ring[kQueueSize]; };
    struct VUsedElem { uint32_t id, len; };
    struct VUsed  { uint16_t flags, idx; VUsedElem ring[kQueueSize]; };

    struct Queue {
        uint32_t num        {kQueueSize};
        uint64_t desc_gpa   {0};
        uint64_t avail_gpa  {0};
        uint64_t used_gpa   {0};
        uint16_t last_avail {0};
        bool     ready      {false};
    };

    void enqueue_rx(const uint8_t* frame, std::size_t len);
    void rx_flush_func();
    void do_feed_rx(const uint8_t* frame, std::size_t len);
    void process_tx();

    VMContext& ctx_;
    std::mutex mtx_;

    uint32_t status_       {0};
    uint32_t queue_sel_    {0};
    uint32_t irq_status_   {0};
    uint32_t dev_feat_sel_ {0};
    uint32_t drv_feat_sel_ {0};
    uint64_t features_;
    uint8_t  mac_[6];

    Queue queues_[2];

    std::unique_ptr<network::Stack> stack_;
    std::queue<std::vector<uint8_t>> rx_queue_;
    std::atomic<bool>                stop_rx_{false};
    std::thread                      rx_thread_;
    std::condition_variable          rx_cv_;
};

} // namespace compute::vm::internal::hvf::virtio