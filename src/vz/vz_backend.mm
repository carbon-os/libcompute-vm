#import "vz_backend.hpp"

#import <Virtualization/Virtualization.h>
#import <Foundation/Foundation.h>

#include <stdexcept>
#include <string>

// ── helpers ───────────────────────────────────────────────────────────────────

static uint64_t parse_size(const std::string& s) {
    // "8GB" → bytes,  "512MB" → bytes
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
        // macOS restore image — caller should use
        // VZMacOSInstaller separately; stub for now
        NSLog(@"[vz] IPSW boot not yet implemented: %s", image_path.c_str());
    } else {
        VZEFIBootLoader* bootLoader = [[VZEFIBootLoader alloc] init];
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

    NSLog(@"[vz] created '%s' → handle %llu", vm_name.c_str(), handle);
    return handle;
}

void VzBackend::UI(uint64_t handle, void* native_handle) {
    if (!native_handle) return;   // headless

    std::lock_guard lock(mutex_);
    auto it = vms_.find(handle);
    if (it == vms_.end()) return;

    VZVirtualMachine* machine =
        (__bridge VZVirtualMachine*)it->second;

    // native_handle is NSView* passed as void* across FFI
    NSView* parentView = (__bridge NSView*)native_handle;

    VZVirtualMachineView* vmView =
        [[VZVirtualMachineView alloc] initWithFrame:parentView.bounds];

    vmView.virtualMachine             = machine;
    vmView.capturesSystemKeys         = YES;
    vmView.autoresizingMask           =
        NSViewWidthSizable | NSViewHeightSizable;

    [parentView addSubview:vmView];

    NSLog(@"[vz] UI attached → handle %llu", handle);
}

void VzBackend::Destroy(uint64_t handle) {
    std::lock_guard lock(mutex_);
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