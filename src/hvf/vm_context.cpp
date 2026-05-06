#include "hvf/vm_context.hpp"
#include "hvf/vcpu.hpp"
#include "hvf/fw_gic.hpp"
#include "hvf/virtio_block.hpp"
#include "hvf/virtio_console.hpp"
#include "hvf/virtio_gpu.hpp"
#include "hvf/virtio_net.hpp"
#include "hvf/virtio_input.hpp"
#include <logger.hpp>
#include <sys/mman.h>
#include <sys/shm.h>
#include <unistd.h>

namespace compute::vm::internal::hvf {

VMContext::VMContext(uint32_t vcpu_count, uint64_t ram_size) noexcept
    : ram_size_(ram_size), vcpu_count_(vcpu_count) {}

VMContext::~VMContext() {
    vcpus_.clear();
    tablet_.reset();
    keyboard_.reset();
    gpu_.reset();
    net_.reset();
    block_.reset();
    console_.reset();

    if (ram_hva_) {
        hv_vm_unmap(ram_gpa_, ram_size_);
        ::munmap(ram_hva_, ram_size_);
        ram_hva_ = nullptr;
    }
    if (!ram_shm_path_.empty()) ::shm_unlink(ram_shm_path_.c_str());
}

void* VMContext::gpa_to_hva(uint64_t gpa) const noexcept {
    if (ram_hva_ && gpa >= ram_gpa_ && gpa < ram_gpa_ + ram_size_)
        return static_cast<uint8_t*>(ram_hva_) + (gpa - ram_gpa_);
    return nullptr;
}

void VMContext::set_ram(void* hva, uint64_t gpa, uint64_t size, std::string shm_path) noexcept {
    ram_hva_ = hva; ram_gpa_ = gpa; ram_size_ = size; ram_shm_path_ = std::move(shm_path);
}

bool VMContext::init_vcpus() {
    vcpus_.reserve(vcpu_count_);
    for (uint32_t i = 0; i < vcpu_count_; ++i)
        vcpus_.push_back(std::make_unique<VCpu>(*this, i));
    return true;
}

bool VMContext::create_console(int rx_fd, int tx_fd) {
    console_ = std::make_unique<virtio::Console>(*this, kGpaVirtioConsole, rx_fd, tx_fd);
    return console_ != nullptr;
}

bool VMContext::create_block(std::string_view image_path) {
    std::string p{image_path};
    block_ = std::make_unique<virtio::Block>(*this, kGpaVirtioBlock, p.c_str());
    if (!block_->valid()) logger::Warn("[blk] no disk image — /dev/vda unavailable\n");
    return true;
}

bool VMContext::create_net(const NetDeviceConfig& cfg) {
    if (!cfg.enabled) return true;
    net_ = std::make_unique<virtio::Net>(*this, kGpaVirtioNet, cfg);
    return true;
}

bool VMContext::create_gpu(uint32_t width, uint32_t height, GpuFrameCallback cb) {
    gpu_ = std::make_unique<virtio::Gpu>(*this, kGpaVirtioGpu, width, height, std::move(cb));
    return true;
}

bool VMContext::create_keyboard() {
    keyboard_ = std::make_unique<virtio::Input>(
        *this, kGpaVirtioKeyboard, kSpiKeyboard,
        virtio::Input::Type::Keyboard);
    return true;
}

bool VMContext::create_tablet(uint32_t width, uint32_t height) {
    tablet_ = std::make_unique<virtio::Input>(
        *this, kGpaVirtioTablet, kSpiTablet,
        virtio::Input::Type::Tablet, width, height);
    return true;
}

void VMContext::run_all() {
    running_.store(true, std::memory_order_relaxed);
    for (auto& v : vcpus_) v->spawn();

    while (running_.load(std::memory_order_relaxed)) {
        ::usleep(1000);
    }

    for (auto& v : vcpus_) v->join();
    running_.store(false, std::memory_order_relaxed);
}

} // namespace compute::vm::internal::hvf