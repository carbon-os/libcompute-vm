#include "hvf/memory.hpp"
#include "hvf/fw_gic.hpp"
#include "hvf/vm_context.hpp"
#include <logger.hpp>
#include <Hypervisor/Hypervisor.h>
#include <os/object.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace compute::vm::internal::hvf {

bool Memory::init(VMContext& ctx) {
    // ── Guest RAM ─────────────────────────────────────────────────────────────
    const std::string shm_path = "/virtualization-ram";
    ::shm_unlink(shm_path.c_str());

    const int fd = ::shm_open(shm_path.c_str(),
                              O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        logger::Error("[mem] shm_open %s: %s\n",
                      shm_path.c_str(), strerror(errno));
        return false;
    }

    if (::ftruncate(fd, static_cast<off_t>(ctx.ram_size())) < 0) {
        logger::Error("[mem] ftruncate: %s\n", strerror(errno));
        ::close(fd);
        ::shm_unlink(shm_path.c_str());
        return false;
    }

    void* hva = ::mmap(nullptr, ctx.ram_size(),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);

    if (hva == MAP_FAILED) {
        logger::Error("[mem] mmap failed: %s\n", strerror(errno));
        ::shm_unlink(shm_path.c_str());
        return false;
    }

    ctx.set_ram(hva, kGpaRamBase, ctx.ram_size(), shm_path);

    HV_CHECK(hv_vm_map(hva, kGpaRamBase, ctx.ram_size(),
                       HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
             "hv_vm_map RAM");

    logger::Info("[mem] RAM: GPA 0x%llx – 0x%llx  (%llu MiB)  shm=%s\n",
                 static_cast<unsigned long long>(kGpaRamBase),
                 static_cast<unsigned long long>(kGpaRamBase + ctx.ram_size()),
                 static_cast<unsigned long long>(ctx.ram_size() / (1024 * 1024)),
                 shm_path.c_str());

    // ── GICv3 ─────────────────────────────────────────────────────────────────
    {
        hv_gic_config_t cfg = hv_gic_config_create();

        HV_CHECK(hv_gic_config_set_distributor_base(cfg, kGpaGicDist),
                 "hv_gic_config_set_distributor_base");
        HV_CHECK(hv_gic_config_set_redistributor_base(cfg, kGpaGicRedist),
                 "hv_gic_config_set_redistributor_base");
        HV_CHECK(hv_gic_create(cfg), "hv_gic_create");

        os_release(cfg);
    }

    const uint64_t gicr_size = kVcpuMax * kGpaGicRedistStride;
    logger::Info("[mem] GICv3: GICD=0x%llx  GICR=0x%llx  "
                 "(stride=0x%llx × %u vCPUs)\n",
                 static_cast<unsigned long long>(kGpaGicDist),
                 static_cast<unsigned long long>(kGpaGicRedist),
                 static_cast<unsigned long long>(kGpaGicRedistStride),
                 kVcpuMax);
    (void)gicr_size;

    return true;
}

} // namespace compute::vm::internal::hvf