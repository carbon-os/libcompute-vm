#include "hvf/virtio_net.hpp"
#include "hvf/fw_gic.hpp"
#include "hvf/vm_context.hpp"
#include <logger.hpp>
#include <algorithm>
#include <cstring>

namespace compute::vm::internal::hvf::virtio {

namespace Reg {
    inline constexpr uint32_t Magic         = 0x000;
    inline constexpr uint32_t Version       = 0x004;
    inline constexpr uint32_t DeviceId      = 0x008;
    inline constexpr uint32_t VendorId      = 0x00C;
    inline constexpr uint32_t DeviceFeat    = 0x010;
    inline constexpr uint32_t DeviceFeatSel = 0x014;
    inline constexpr uint32_t DriverFeat    = 0x020;
    inline constexpr uint32_t DriverFeatSel = 0x024;
    inline constexpr uint32_t QueueSel      = 0x030;
    inline constexpr uint32_t QueueNumMax   = 0x034;
    inline constexpr uint32_t QueueNum      = 0x038;
    inline constexpr uint32_t QueueReady    = 0x044;
    inline constexpr uint32_t QueueNotify   = 0x050;
    inline constexpr uint32_t IrqStatus     = 0x060;
    inline constexpr uint32_t IrqAck        = 0x064;
    inline constexpr uint32_t Status        = 0x070;
    inline constexpr uint32_t QueueDescLo   = 0x080;
    inline constexpr uint32_t QueueDescHi   = 0x084;
    inline constexpr uint32_t QueueAvailLo  = 0x090;
    inline constexpr uint32_t QueueAvailHi  = 0x094;
    inline constexpr uint32_t QueueUsedLo   = 0x0A0;
    inline constexpr uint32_t QueueUsedHi   = 0x0A4;
    inline constexpr uint32_t ConfigGen     = 0x0FC;
    inline constexpr uint32_t Config        = 0x100;
}

inline constexpr uint64_t kVirtioF_Version1 = 1ULL << 32;
inline constexpr uint64_t kNetF_Mac         = 1ULL << 5;

Net::Net(VMContext& ctx, uint64_t /*gpa*/, const NetDeviceConfig& cfg) noexcept
    : ctx_(ctx), features_(kVirtioF_Version1 | kNetF_Mac)
{
    std::memcpy(mac_, cfg.mac, 6);

    network::Config net_cfg;
    std::memcpy(net_cfg.guest_mac, mac_, 6);
    for (const auto& pf : cfg.port_forwards) {
        net_cfg.port_forwards.push_back({pf.host_port, pf.guest_port});
    }

    stack_ = std::make_unique<network::Stack>(
        std::move(net_cfg),
        [this](const uint8_t* frame, size_t len) { this->enqueue_rx(frame, len); }
    );

    rx_thread_ = std::thread(&Net::rx_flush_func, this);
}

Net::~Net() {
    stop_rx_.store(true, std::memory_order_relaxed);
    rx_cv_.notify_all();
    if (rx_thread_.joinable()) rx_thread_.join();
}

void Net::notify_irq() { raise_spi(kSpiNet); }

void Net::enqueue_rx(const uint8_t* frame, std::size_t len) {
    std::lock_guard<std::mutex> lk(mtx_);
    rx_queue_.push(std::vector<uint8_t>(frame, frame + len));
    rx_cv_.notify_one();
}

void Net::rx_flush_func() {
    while (!stop_rx_.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            rx_cv_.wait(lk, [this] { 
                return stop_rx_.load(std::memory_order_relaxed) || !rx_queue_.empty(); 
            });
            
            if (stop_rx_.load(std::memory_order_relaxed)) break;
            
            frame = std::move(rx_queue_.front());
            rx_queue_.pop();
        }
        
        std::lock_guard<std::mutex> lk(mtx_);
        do_feed_rx(frame.data(), frame.size());
    }
}

void Net::do_feed_rx(const uint8_t* frame, std::size_t len) {
    Queue& q = queues_[0];
    if (!q.ready || !q.desc_gpa || !q.avail_gpa || !q.used_gpa) return;

    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!desc || !avail || !used || q.last_avail == avail->idx) return;

    const uint16_t head = avail->ring[q.last_avail % kQueueSize];
    uint16_t       cur  = head;
    uint32_t       wrote = 0;

    // 1. Write virtio_net_hdr (empty, no GSO/csum offload for now)
    void* buf = ctx_.gpa_to_hva(desc[cur].addr);
    if (buf && desc[cur].len >= sizeof(VirtioNetHdr)) {
        std::memset(buf, 0, sizeof(VirtioNetHdr));
        wrote += sizeof(VirtioNetHdr);
    }
    
    // 2. Write frame data into remaining descriptor(s)
    size_t frame_offset = 0;
    while (frame_offset < len) {
        if (!buf) break; // Invalid GPA
        
        const size_t cap = desc[cur].len - (wrote > 0 && cur == head ? sizeof(VirtioNetHdr) : 0);
        const size_t to_copy = std::min(len - frame_offset, cap);
        
        if (to_copy > 0) {
            uint8_t* dst = static_cast<uint8_t*>(buf) + (wrote > 0 && cur == head ? sizeof(VirtioNetHdr) : 0);
            std::memcpy(dst, frame + frame_offset, to_copy);
            wrote += to_copy;
            frame_offset += to_copy;
        }

        if (frame_offset < len && (desc[cur].flags & 1)) {
            cur = desc[cur].next;
            buf = ctx_.gpa_to_hva(desc[cur].addr);
        } else {
            break;
        }
    }

    const uint16_t ui  = used->idx % kQueueSize;
    used->ring[ui].id  = head;
    used->ring[ui].len = wrote;
    __atomic_store_n(&used->idx, static_cast<uint16_t>(used->idx + 1), __ATOMIC_RELEASE);
    
    ++q.last_avail;
    irq_status_ |= 1;
    notify_irq();
}

void Net::process_tx() {
    Queue& q = queues_[1];
    if (!q.ready || !q.desc_gpa || !q.avail_gpa || !q.used_gpa) return;

    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!desc || !avail || !used) return;

    bool raised = false;
    std::vector<uint8_t> frame_buf;

    while (q.last_avail != avail->idx) {
        const uint16_t head = avail->ring[q.last_avail % kQueueSize];
        uint16_t       cur  = head;
        frame_buf.clear();

        do {
            if (void* buf = ctx_.gpa_to_hva(desc[cur].addr)) {
                auto* b8 = static_cast<const uint8_t*>(buf);
                frame_buf.insert(frame_buf.end(), b8, b8 + desc[cur].len);
            }
            if (!(desc[cur].flags & 1)) break;
            cur = desc[cur].next;
        } while (true);

        if (frame_buf.size() > sizeof(VirtioNetHdr)) {
            const uint8_t* payload = frame_buf.data() + sizeof(VirtioNetHdr);
            const size_t   plen    = frame_buf.size() - sizeof(VirtioNetHdr);
            if (stack_) stack_->guest_tx(payload, plen);
        }

        const uint16_t ui  = used->idx % kQueueSize;
        used->ring[ui].id  = head;
        used->ring[ui].len = frame_buf.size();
        __atomic_store_n(&used->idx, static_cast<uint16_t>(used->idx + 1), __ATOMIC_RELEASE);
        
        ++q.last_avail;
        raised = true;
    }

    if (raised) {
        irq_status_ |= 1;
        notify_irq();
    }
}

uint32_t Net::read(uint32_t off, uint32_t len) {
    std::lock_guard<std::mutex> lk(mtx_);
    const Queue* q = (queue_sel_ < 2) ? &queues_[queue_sel_] : nullptr;

    if (off >= Reg::Config && off < Reg::Config + 6) {
        uint32_t val = 0;
        std::memcpy(&val, &mac_[off - Reg::Config], std::min(len, 6u - (off - Reg::Config)));
        return val;
    }

    switch (off) {
        case Reg::Magic:       return 0x74726976U;
        case Reg::Version:     return 2;
        case Reg::DeviceId:    return 1; // virtio-net
        case Reg::VendorId:    return 0x554D4551U;
        case Reg::DeviceFeat:
            if (dev_feat_sel_ == 0) return static_cast<uint32_t>(features_);
            if (dev_feat_sel_ == 1) return static_cast<uint32_t>(features_ >> 32);
            return 0;
        case Reg::QueueNumMax: return kQueueSize;
        case Reg::QueueReady:  return (q && q->ready) ? 1u : 0u;
        case Reg::IrqStatus:   return irq_status_;
        case Reg::Status:      return status_;
        case Reg::ConfigGen:   return 0;
        default:               return 0;
    }
}

void Net::write(uint32_t off, uint32_t val, uint32_t) {
    std::lock_guard<std::mutex> lk(mtx_);
    Queue* q = (queue_sel_ < 2) ? &queues_[queue_sel_] : nullptr;

#define LO(f, v) if(q) q->f = (q->f & 0xFFFFFFFF00000000ULL) | (v)
#define HI(f, v) if(q) q->f = (q->f & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(v) << 32)

    switch (off) {
        case Reg::DeviceFeatSel: dev_feat_sel_ = val; break;
        case Reg::DriverFeatSel: drv_feat_sel_ = val; break;
        case Reg::DriverFeat:
            if (drv_feat_sel_ == 0)
                features_ = (features_ & 0xFFFFFFFF00000000ULL) | val;
            else if (drv_feat_sel_ == 1)
                features_ = (features_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(val) << 32);
            break;
        case Reg::QueueSel:    queue_sel_ = val; break;
        case Reg::QueueNum:    if (q) q->num = std::min(val, kQueueSize); break;
        case Reg::QueueReady:  if (q) q->ready = (val != 0); break;
        case Reg::QueueNotify:
            if (val == 1) process_tx();
            break;
        case Reg::IrqAck:      irq_status_ &= ~val; break;
        case Reg::Status:
            status_ = val;
            if (val == 0) {
                queues_[0] = queues_[1] = Queue{};
                irq_status_ = 0;
            }
            break;
        case Reg::QueueDescLo:  LO(desc_gpa,  val); break;
        case Reg::QueueDescHi:  HI(desc_gpa,  val); break;
        case Reg::QueueAvailLo: LO(avail_gpa, val); break;
        case Reg::QueueAvailHi: HI(avail_gpa, val); break;
        case Reg::QueueUsedLo:  LO(used_gpa,  val); break;
        case Reg::QueueUsedHi:  HI(used_gpa,  val); break;
        default: break;
    }
#undef LO
#undef HI
}

} // namespace compute::vm::internal::hvf::virtio