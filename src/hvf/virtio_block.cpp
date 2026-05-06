#include "hvf/virtio_block.hpp"
#include "hvf/fw_gic.hpp"
#include "hvf/vm_context.hpp"
#include <logger.hpp>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
} // namespace Reg

// ── Virtio descriptor flags ───────────────────────────────────────────────────
inline constexpr uint16_t VRING_DESC_F_NEXT  = 1;
inline constexpr uint16_t VRING_DESC_F_WRITE = 2;

// ── virtio_blk_config field offsets (relative to Reg::Config) ────────────────
// Ref: virtio spec 1.2 §5.2.4
//   +0x00  u64  capacity        (in 512-byte sectors)
//   +0x08  u32  size_max        (max bytes in a single segment)
//   +0x0C  u32  seg_max         (max number of segments per request)
//   +0x10  struct geometry      (cylinders u16, heads u8, sectors u8)
//   +0x14  u32  blk_size        (physical block size in bytes)
namespace BlkCfg {
    inline constexpr uint32_t CapacityLo = 0x00;
    inline constexpr uint32_t CapacityHi = 0x04;
    inline constexpr uint32_t SizeMax    = 0x08;
    inline constexpr uint32_t SegMax     = 0x0C;
    inline constexpr uint32_t Geometry   = 0x10;
    inline constexpr uint32_t BlkSize    = 0x14;
} // namespace BlkCfg

inline constexpr uint64_t kVirtioF_Version1 = 1ULL << 32;
inline constexpr uint64_t kBlkF_SizeMax     = 1ULL << 1;
inline constexpr uint64_t kBlkF_SegMax      = 1ULL << 2;
inline constexpr uint64_t kBlkF_BlkSize     = 1ULL << 6;
inline constexpr uint64_t kBlkF_Flush       = 1ULL << 9;

// Block device config values.
// seg_max = kQueueSize - 2 ensures one max-sized request (seg_max data
// descriptors + 1 header + 1 status) always fits inside the descriptor ring.
inline constexpr uint32_t kSizeMax = 65536u;               // max bytes per segment (64 KiB)
inline constexpr uint32_t kSegMax  = 254u; // kQueueSize(256) - 2 (header + status)
inline constexpr uint32_t kBlkSize = 512u;                  // physical sector size

struct virtio_blk_req {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
} __attribute__((packed));

inline constexpr uint32_t VIRTIO_BLK_T_IN     = 0;
inline constexpr uint32_t VIRTIO_BLK_T_OUT    = 1;
inline constexpr uint32_t VIRTIO_BLK_T_FLUSH  = 4;
inline constexpr uint32_t VIRTIO_BLK_T_GET_ID = 8;

inline constexpr uint8_t VIRTIO_BLK_S_OK     = 0;
inline constexpr uint8_t VIRTIO_BLK_S_IOERR  = 1;
inline constexpr uint8_t VIRTIO_BLK_S_UNSUPP = 2;

Block::Block(VMContext& ctx, uint64_t /*gpa*/, const char* image_path) noexcept
    : ctx_(ctx), features_(kVirtioF_Version1 | kBlkF_Flush | kBlkF_SegMax | kBlkF_SizeMax | kBlkF_BlkSize)
{
    fd_ = ::open(image_path, O_RDWR | O_CREAT, 0644);
    if (fd_ >= 0) {
        struct stat st;
        if (::fstat(fd_, &st) == 0) {
            capacity_ = static_cast<uint64_t>(st.st_size) / 512;
            logger::Info("[blk] opened %s: %llu sectors (%llu MiB)\n",
                         image_path,
                         static_cast<unsigned long long>(capacity_),
                         static_cast<unsigned long long>(st.st_size / (1024 * 1024)));
        }
    } else {
        logger::Error("[blk] failed to open %s: %s\n", image_path, strerror(errno));
    }
}

Block::~Block() {
    if (fd_ >= 0) ::close(fd_);
}

void Block::notify_irq() { raise_spi(kSpiBlock); }

void Block::process_rq() {
    Queue& q = queue_;
    if (!q.ready || !q.desc_gpa || !q.avail_gpa || !q.used_gpa) return;

    auto* desc  = static_cast<VDesc*> (ctx_.gpa_to_hva(q.desc_gpa));
    auto* avail = static_cast<VAvail*>(ctx_.gpa_to_hva(q.avail_gpa));
    auto* used  = static_cast<VUsed*> (ctx_.gpa_to_hva(q.used_gpa));
    if (!desc || !avail || !used) return;

    bool raised = false;

    while (q.last_avail != avail->idx) {
        const uint16_t head = avail->ring[q.last_avail % kQueueSize];
        uint16_t cur = head;

        // ── Descriptor 0: request header ─────────────────────────────────────
        const auto* req = static_cast<const virtio_blk_req*>(ctx_.gpa_to_hva(desc[cur].addr));
        if (!req || desc[cur].len < sizeof(virtio_blk_req)) {
            logger::Warn("[blk] bad request descriptor at head=%u\n", head);
            break;
        }
        if (!(desc[cur].flags & VRING_DESC_F_NEXT)) {
            logger::Warn("[blk] request descriptor chain too short at head=%u\n", head);
            break;
        }
        cur = desc[cur].next;

        const uint32_t type   = req->type;
        uint64_t       off    = req->sector * 512;
        uint8_t        status = VIRTIO_BLK_S_OK;
        uint32_t       wrote  = 0;

        // ── Descriptor(s): data buffers ───────────────────────────────────────
        if (type == VIRTIO_BLK_T_FLUSH) {
            ::fsync(fd_);
            // No data descriptors; cur already points at the status descriptor.
        } else if (type == VIRTIO_BLK_T_GET_ID) {
            void* buf = ctx_.gpa_to_hva(desc[cur].addr);
            if (buf && desc[cur].len >= 20) {
                std::strncpy(static_cast<char*>(buf), "vda", 20);
                wrote += desc[cur].len;
            }
            // Advance past the data descriptor to reach the status descriptor.
            if (desc[cur].flags & VRING_DESC_F_NEXT)
                cur = desc[cur].next;
        } else {
            // Walk the chain of data descriptors (each has NEXT set).
            // The final data descriptor points to the status descriptor which
            // has no NEXT flag — that terminates the loop.
            while (desc[cur].flags & VRING_DESC_F_NEXT) {
                void*          buf = ctx_.gpa_to_hva(desc[cur].addr);
                const uint32_t len = desc[cur].len;
                if (buf && len) {
                    if (type == VIRTIO_BLK_T_IN) {
                        if (::pread(fd_, buf, len, static_cast<off_t>(off)) < 0)
                            status = VIRTIO_BLK_S_IOERR;
                        wrote += len;
                    } else if (type == VIRTIO_BLK_T_OUT) {
                        if (::pwrite(fd_, buf, len, static_cast<off_t>(off)) < 0)
                            status = VIRTIO_BLK_S_IOERR;
                    } else {
                        status = VIRTIO_BLK_S_UNSUPP;
                    }
                    off += len;
                }
                cur = desc[cur].next;
            }
        }

        // ── Last descriptor: status byte ──────────────────────────────────────
        if (void* status_ptr = ctx_.gpa_to_hva(desc[cur].addr)) {
            *static_cast<uint8_t*>(status_ptr) = status;
            wrote += 1;
        }

        const uint16_t ui = used->idx % kQueueSize;
        used->ring[ui].id  = head;
        used->ring[ui].len = wrote;
        __atomic_store_n(&used->idx, static_cast<uint16_t>(used->idx + 1), __ATOMIC_RELEASE);

        ++q.last_avail;
        raised = true;
    }

    if (raised) {
        irq_status_ |= 1;
        notify_irq();
    }
}

uint32_t Block::read(uint32_t off, uint32_t /*len*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    const Queue* q = &queue_;

    // ── virtio_blk_config ─────────────────────────────────────────────────────
    if (off >= Reg::Config) {
        const uint32_t cfg = off - Reg::Config;
        switch (cfg) {
            case BlkCfg::CapacityLo: return static_cast<uint32_t>(capacity_);
            case BlkCfg::CapacityHi: return static_cast<uint32_t>(capacity_ >> 32);
            case BlkCfg::SizeMax:    return kSizeMax;
            case BlkCfg::SegMax:     return kSegMax;
            case BlkCfg::Geometry:   return 0;
            case BlkCfg::BlkSize:    return kBlkSize;
            default:                 return 0;
        }
    }

    // ── Standard virtio-mmio registers ───────────────────────────────────────
    switch (off) {
        case Reg::Magic:       return 0x74726976U;
        case Reg::Version:     return 2;
        case Reg::DeviceId:    return 2; // virtio-blk
        case Reg::VendorId:    return 0x554D4551U;
        case Reg::DeviceFeat:
            if (dev_feat_sel_ == 0) return static_cast<uint32_t>(features_);
            if (dev_feat_sel_ == 1) return static_cast<uint32_t>(features_ >> 32);
            return 0;
        case Reg::QueueNumMax: return kQueueSize;
        case Reg::QueueReady:  return q->ready ? 1u : 0u;
        case Reg::IrqStatus:   return irq_status_;
        case Reg::Status:      return status_;
        case Reg::ConfigGen:   return 0;
        default:               return 0;
    }
}

void Block::write(uint32_t off, uint32_t val, uint32_t /*len*/) {
    std::lock_guard<std::mutex> lk(mtx_);
    Queue* q = &queue_;

#define LO(f, v) q->f = (q->f & 0xFFFFFFFF00000000ULL) | (v)
#define HI(f, v) q->f = (q->f & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(v) << 32)

    switch (off) {
        case Reg::DeviceFeatSel: dev_feat_sel_ = val; break;
        case Reg::DriverFeatSel: drv_feat_sel_ = val; break;
        case Reg::DriverFeat:
            if (drv_feat_sel_ == 0)
                features_ = (features_ & 0xFFFFFFFF00000000ULL) | val;
            else if (drv_feat_sel_ == 1)
                features_ = (features_ & 0x00000000FFFFFFFFULL)
                            | (static_cast<uint64_t>(val) << 32);
            break;
        case Reg::QueueSel:    break; // single queue; ignore selector
        case Reg::QueueNum:    q->num = std::min(val, kQueueSize); break;
        case Reg::QueueReady:  q->ready = (val != 0); break;
        case Reg::QueueNotify: if (val == 0) process_rq(); break;
        case Reg::IrqAck:      irq_status_ &= ~val; break;
        case Reg::Status:
            status_ = val;
            if (val == 0) { queue_ = Queue{}; irq_status_ = 0; }
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