#include "hvf/hvf_backend.hpp"
#include "hvf/fw_dtb.hpp"
#include "hvf/fw_loader.hpp"
#include "hvf/memory.hpp"
#include <logger.hpp>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <Hypervisor/Hypervisor.h>

namespace compute::vm::internal::hvf {

static uint64_t parse_size(const std::string& s) {
    size_t pos = 0;
    uint64_t val = std::stoull(s, &pos);
    std::string unit = s.substr(pos);
    if (unit == "GB") return val * 1024ULL * 1024 * 1024;
    if (unit == "MB") return val * 1024ULL * 1024;
    return val;
}

uint64_t HvfBackend::Create(const nlohmann::json& config) {
    // ── config ────────────────────────────────────────────────
    int         cpu_count  = config.value("cpu_count", 1);
    uint64_t    ram_bytes  = parse_size(config.value("ram_size",  "2GB"));
    std::string image_path = config.value("image", "");
    std::string vm_name    = config.value("name",  "vm");

    auto ctx = std::make_unique<VMContext>(cpu_count, ram_bytes);

    if (config.contains("cmdline"))
        ctx->set_cmdline(config.value("cmdline", ""));

    if (hv_vm_create(nullptr) != HV_SUCCESS)
        throw std::runtime_error("hvf: hv_vm_create failed");

    if (!Memory::init(*ctx))
        throw std::runtime_error("hvf: Memory::init failed");

    // ── serial ports / IPC ────────────────────────────────────
    std::vector<SerialChannel> serial_channels;

    if (config.contains("serial") && !config["serial"].empty()) {
        const auto& serial_arr = config["serial"];
        std::vector<nlohmann::json> sorted_serial(serial_arr.begin(), serial_arr.end());
        std::sort(sorted_serial.begin(), sorted_serial.end(),
            [](const auto& a, const auto& b) {
                return a.value("port", 0) < b.value("port", 0);
            });

        for (const auto& scfg : sorted_serial) {
            int         port_idx     = scfg.value("port", 0);
            std::string channel_name = scfg.value("channel", "");

            int to_vm[2], from_vm[2];
            if (::pipe(to_vm)   != 0) throw std::runtime_error("hvf: pipe() failed");
            if (::pipe(from_vm) != 0) throw std::runtime_error("hvf: pipe() failed");

            if (port_idx == 0) {
                ctx->set_pl011_tx_fd(from_vm[1]);
                ::close(to_vm[0]);
            } else {
                ::close(to_vm[0]);
                ::close(from_vm[1]);
            }

            SerialChannel ch;
            ch.host_write_fd = to_vm[1];
            ch.host_read_fd  = from_vm[0];
            ch.server        = std::make_unique<ipc::Server>(channel_name);

            ch.server->on_message([wfd = ch.host_write_fd](ipc::Message msg) {
                auto data = msg.binary();
                ::write(wfd, data.data(), data.size());
            });
            ch.server->listen();

            int          rfd = ch.host_read_fd;
            ipc::Server* srv = ch.server.get();
            ch.pump_thread = std::jthread([rfd, srv](std::stop_token st) {
                uint8_t buf[4096];
                while (!st.stop_requested()) {
                    ssize_t n = ::read(rfd, buf, sizeof(buf));
                    if (n <= 0) break;
                    srv->send(std::vector<uint8_t>(buf, buf + n));
                }
            });

            logger::Info("[hvf] serial port %d → ipc channel '%s'\n",
                         port_idx, channel_name.c_str());
            serial_channels.push_back(std::move(ch));
        }
    }

    // ── virtio console ────────────────────────────────────────
    if (!ctx->create_console(-1, -1))
        throw std::runtime_error("hvf: create_console failed");

    // ── block device ──────────────────────────────────────────
    if (!image_path.empty()) {
        if (!ctx->create_block(image_path))
            throw std::runtime_error("hvf: create_block failed");
    }

    // ── network ───────────────────────────────────────────────
    if (config.contains("network") && config["network"].value("enabled", false)) {
        NetDeviceConfig net_cfg;
        net_cfg.enabled = true;

        if (config["network"].contains("port_forwards")) {
            for (const auto& pf : config["network"]["port_forwards"]) {
                net_cfg.port_forwards.push_back({
                    static_cast<uint16_t>(pf.value("host",  0)),
                    static_cast<uint16_t>(pf.value("guest", 0))
                });
            }
        }

        if (!ctx->create_net(net_cfg))
            throw std::runtime_error("hvf: create_net failed");

        logger::Info("[hvf] virtio-net enabled (NAT 10.0.2.x)%s\n",
                     net_cfg.port_forwards.empty() ? "" : " with port forwards");
    }

    // ── bootloader ────────────────────────────────────────────
    std::string kernel_path = config.value("kernel", "");
    std::string initrd_path = config.value("initrd", "");

    if (!kernel_path.empty()) {
        if (!ctx->init_vcpus())           throw std::runtime_error("hvf: init_vcpus failed");
        if (!Loader::load_kernel(*ctx, kernel_path)) throw std::runtime_error("hvf: load_kernel failed");
        if (!initrd_path.empty()) {
            if (!Loader::load_initrd(*ctx, initrd_path)) throw std::runtime_error("hvf: load_initrd failed");
        }
        if (!DTB::generate(*ctx))         throw std::runtime_error("hvf: DTB::generate failed");
    } else {
        throw std::runtime_error("hvf: missing kernel (UEFI not supported in custom HVF yet)");
    }

    // ── start execution ───────────────────────────────────────
    Instance inst;
    inst.serial = std::move(serial_channels);
    inst.run_thread = std::thread([ctx_ptr = ctx.get()] {
        ctx_ptr->run_all();
    });
    inst.ctx = std::move(ctx);

    std::lock_guard lock(mutex_);
    uint64_t handle = next_id_++;
    vms_[handle] = std::move(inst);

    logger::Info("[hvf] created '%s' → handle %llu\n",
                 vm_name.c_str(), static_cast<unsigned long long>(handle));
    return handle;
}

void HvfBackend::UI(uint64_t handle, void* native_handle) {
    logger::Warn("[hvf] UI mapping requested for handle %llu, "
                 "but custom HVF Metal rendering is not yet wired up.\n",
                 static_cast<unsigned long long>(handle));
}

void HvfBackend::Destroy(uint64_t handle) {
    std::lock_guard lock(mutex_);
    auto it = vms_.find(handle);
    if (it == vms_.end()) return;

    for (auto& ch : it->second.serial) {
        if (ch.host_read_fd  >= 0) { ::close(ch.host_read_fd);  ch.host_read_fd  = -1; }
        if (ch.host_write_fd >= 0) { ::close(ch.host_write_fd); ch.host_write_fd = -1; }
    }

    it->second.ctx->stop();
    if (it->second.run_thread.joinable())
        it->second.run_thread.join();

    it->second.ctx.reset();
    hv_vm_destroy();

    vms_.erase(it);
    logger::Info("[hvf] destroyed handle %llu\n",
                 static_cast<unsigned long long>(handle));
}

} // namespace compute::vm::internal::hvf