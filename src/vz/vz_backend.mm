// src/vz/vz_backend.mm

#import "vz_backend.hpp"

#import <Virtualization/Virtualization.h>
#import <Foundation/Foundation.h>

#include <stdexcept>
#include <string>
#include <unistd.h>

// ── helpers ───────────────────────────────────────────────────────────────────

static uint64_t parse_size(const std::string& s) {
    size_t pos = 0;
    uint64_t val = std::stoull(s, &pos);
    std::string unit = s.substr(pos);
    if (unit == "GB") return val * 1024ULL * 1024 * 1024;
    if (unit == "MB") return val * 1024ULL * 1024;
    return val;
}

static bool is_ipsw(const std::string& path) {
    return path.size() >= 5 &&
           path.substr(path.size() - 5) == ".ipsw";
}

// ── VzBackend ─────────────────────────────────────────────────────────────────

namespace compute::vm::internal {

uint64_t VzBackend::Create(const nlohmann::json& config) {

    NSError* error = nil;

    // ── config ────────────────────────────────────────────────
    int         cpu_count  = config.value("cpu_count", 1);
    uint64_t    ram_bytes  = parse_size(config.value("ram_size",  "2GB"));
    uint64_t    disk_bytes = parse_size(config.value("disk_size", "20GB"));
    std::string image_path = config.value("image", "");
    std::string vm_name    = config.value("name",  "vm");

    // ── display ───────────────────────────────────────────────
    int display_w = 1920;
    int display_h = 1080;
    if (config.contains("display")) {
        display_w = config["display"].value("width",  1920);
        display_h = config["display"].value("height", 1080);
    }

    // ── VZVirtualMachineConfiguration ────────────────────────
    VZVirtualMachineConfiguration* vmConfig =
        [[VZVirtualMachineConfiguration alloc] init];

    vmConfig.CPUCount   = cpu_count;
    vmConfig.memorySize = ram_bytes;

    // ── bootloader ────────────────────────────────────────────
    if (is_ipsw(image_path)) {
        NSLog(@"[vz] IPSW boot not yet implemented: %s", image_path.c_str());
    } else {
        VZEFIBootLoader* bootLoader = [[VZEFIBootLoader alloc] init];

        NSString* nsImagePath = [NSString stringWithUTF8String:image_path.c_str()];
        NSString* nvramPath   = [[nsImagePath stringByDeletingPathExtension]
                                    stringByAppendingString:@".nvram"];
        NSURL*    nvramURL    = [NSURL fileURLWithPath:nvramPath];

        NSFileManager* fm = [NSFileManager defaultManager];
        if ([fm fileExistsAtPath:nvramPath]) {
            bootLoader.variableStore =
                [[VZEFIVariableStore alloc] initWithURL:nvramURL];
        } else {
            bootLoader.variableStore =
                [[VZEFIVariableStore alloc]
                    initCreatingVariableStoreAtURL:nvramURL
                                           options:0
                                             error:&error];
            if (error) {
                throw std::runtime_error(
                    std::string("vz: failed to create EFI variable store: ") +
                    error.localizedDescription.UTF8String);
            }
        }

        vmConfig.bootLoader = bootLoader;
    }

    // ── disk image ────────────────────────────────────────────
    if (!image_path.empty()) {
        NSString* nsPath = [NSString stringWithUTF8String:image_path.c_str()];
        NSURL*    url    = [NSURL fileURLWithPath:nsPath];

        VZDiskImageStorageDeviceAttachment* attachment =
            [[VZDiskImageStorageDeviceAttachment alloc]
                initWithURL:url
                   readOnly:NO
                      error:&error];

        if (error) {
            throw std::runtime_error(
                std::string("vz: disk attachment failed: ") +
                error.localizedDescription.UTF8String);
        }

        VZVirtioBlockDeviceConfiguration* disk =
            [[VZVirtioBlockDeviceConfiguration alloc]
                initWithAttachment:attachment];

        vmConfig.storageDevices = @[disk];
    }

    // ── display (2d) ──────────────────────────────────────────
    if (config.contains("display")) {
        VZVirtioGraphicsDeviceConfiguration* graphics =
            [[VZVirtioGraphicsDeviceConfiguration alloc] init];

        VZVirtioGraphicsScanoutConfiguration* scanout =
            [[VZVirtioGraphicsScanoutConfiguration alloc]
                initWithWidthInPixels:display_w
                      heightInPixels:display_h];

        graphics.scanouts = @[scanout];
        vmConfig.graphicsDevices = @[graphics];
    }

    // ── network ───────────────────────────────────────────────
    VZNATNetworkDeviceAttachment*         nat =
        [[VZNATNetworkDeviceAttachment alloc] init];
    VZVirtioNetworkDeviceConfiguration* net =
        [[VZVirtioNetworkDeviceConfiguration alloc] init];
    net.attachment      = nat;
    vmConfig.networkDevices = @[net];

    // ── serial ports ──────────────────────────────────────────
    std::vector<SerialChannel> serial_channels;

    if (config.contains("serial") && !config["serial"].empty()) {
        const auto& serial_arr = config["serial"];

        // Sort by port index so we addObject: in order
        std::vector<nlohmann::json> sorted_serial(serial_arr.begin(), serial_arr.end());
        std::sort(sorted_serial.begin(), sorted_serial.end(),
            [](const auto& a, const auto& b) {
                return a.value("port", 0) < b.value("port", 0);
            });

        VZVirtioConsoleDeviceConfiguration* consoleDev =
            [[VZVirtioConsoleDeviceConfiguration alloc] init];

        serial_channels.reserve(sorted_serial.size());

        for (const auto& scfg : sorted_serial) {
            int         port_idx     = scfg.value("port", 0);
            std::string channel_name = scfg.value("channel", "");

            // to_vm:   host writes [1], VZ reads  [0]
            // from_vm: VZ writes   [1], host reads [0]
            int to_vm[2], from_vm[2];
            if (::pipe(to_vm)   != 0) throw std::runtime_error("vz: pipe() failed");
            if (::pipe(from_vm) != 0) throw std::runtime_error("vz: pipe() failed");

            // Use initWithFileDescriptor:closeOnDealloc: — the correct API
            NSFileHandle* vzRead  = [[NSFileHandle alloc] initWithFileDescriptor:to_vm[0]
                                                                  closeOnDealloc:YES];
            NSFileHandle* vzWrite = [[NSFileHandle alloc] initWithFileDescriptor:from_vm[1]
                                                                  closeOnDealloc:YES];

            VZFileHandleSerialPortAttachment* attachment =
                [[VZFileHandleSerialPortAttachment alloc]
                    initWithFileHandleForReading:vzRead
                             fileHandleForWriting:vzWrite];

            VZVirtioConsolePortConfiguration* portCfg =
                [[VZVirtioConsolePortConfiguration alloc] init];
            portCfg.attachment = attachment;
            portCfg.isConsole  = (port_idx == 0); // hvc0 = kernel console

            // ports is a readonly NSMutableArray — assign via subscript
            consoleDev.ports[port_idx] = portCfg;

            SerialChannel ch;
            ch.host_write_fd = to_vm[1];
            ch.host_read_fd  = from_vm[0];
            ch.server        = std::make_unique<ipc::Server>(channel_name);

            // IPC client → VM: write raw bytes into the pipe
            ch.server->on_message([wfd = ch.host_write_fd](ipc::Message msg) {
                auto data = msg.binary();
                ::write(wfd, data.data(), data.size());
            });
            ch.server->listen();

            // VM → IPC client: pump pipe output as binary frames
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

            NSLog(@"[vz] serial port %d → ipc channel '%s'",
                  port_idx, channel_name.c_str());

            serial_channels.push_back(std::move(ch));
        }

        vmConfig.consoleDevices = @[consoleDev];
    }

    // ── validate ──────────────────────────────────────────────
    if (![vmConfig validateWithError:&error]) {
        throw std::runtime_error(
            std::string("vz: invalid config: ") +
            error.localizedDescription.UTF8String);
    }

    // ── create + start ────────────────────────────────────────
    VZVirtualMachine* machine =
        [[VZVirtualMachine alloc] initWithConfiguration:vmConfig];

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block NSError* startError = nil;

    [machine startWithCompletionHandler:^(NSError* err) {
        startError = err;
        dispatch_semaphore_signal(sem);
    }];

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    if (startError) {
        throw std::runtime_error(
            std::string("vz: start failed: ") +
            startError.localizedDescription.UTF8String);
    }

    // ── store + return handle ─────────────────────────────────
    std::lock_guard lock(mutex_);
    uint64_t handle = next_id_++;
    vms_[handle] = (__bridge_retained void*)machine;

    if (!serial_channels.empty())
        serial_channels_[handle] = std::move(serial_channels);

    NSLog(@"[vz] created '%s' → handle %llu", vm_name.c_str(), handle);
    return handle;
}

void VzBackend::UI(uint64_t handle, void* native_handle) {
    if (!native_handle) return;

    std::lock_guard lock(mutex_);
    auto it = vms_.find(handle);
    if (it == vms_.end()) return;

    VZVirtualMachine* machine =
        (__bridge VZVirtualMachine*)it->second;

    NSView* parentView = (__bridge NSView*)native_handle;

    VZVirtualMachineView* vmView =
        [[VZVirtualMachineView alloc] initWithFrame:parentView.bounds];

    vmView.virtualMachine     = machine;
    vmView.capturesSystemKeys = YES;
    vmView.autoresizingMask   = NSViewWidthSizable | NSViewHeightSizable;

    [parentView addSubview:vmView];

    NSLog(@"[vz] UI attached → handle %llu", handle);
}

void VzBackend::Destroy(uint64_t handle) {
    std::lock_guard lock(mutex_);

    // ── serial cleanup ────────────────────────────────────────
    auto sit = serial_channels_.find(handle);
    if (sit != serial_channels_.end()) {
        for (auto& ch : sit->second) {
            // closing read fd unblocks the pump thread's ::read()
            if (ch.host_read_fd  >= 0) { ::close(ch.host_read_fd);  ch.host_read_fd  = -1; }
            if (ch.host_write_fd >= 0) { ::close(ch.host_write_fd); ch.host_write_fd = -1; }
            // jthread dtor: request_stop() + join() — before server dtor
        }
        serial_channels_.erase(sit);
    }

    // ── vm stop ───────────────────────────────────────────────
    auto it = vms_.find(handle);
    if (it == vms_.end()) return;

    VZVirtualMachine* machine =
        (__bridge_transfer VZVirtualMachine*)it->second;

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block NSError* error   = nil;

    [machine stopWithCompletionHandler:^(NSError* err) {
        error = err;
        dispatch_semaphore_signal(sem);
    }];

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    vms_.erase(it);
    NSLog(@"[vz] destroyed handle %llu", handle);
}

} // namespace compute::vm::internal