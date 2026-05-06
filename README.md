# libcompute-vm

A lightweight C++23 library for running ARM64 virtual machines on Apple Silicon
Macs. Provides a thin, engine-agnostic API over two backends:

- **HVF** — a custom hypervisor built directly on Apple's `Hypervisor.framework`,
  with a full virtio device stack implemented in userspace.
- **VZ** — a wrapper around Apple's higher-level `Virtualization.framework`.

---

## Features

- Direct Linux boot (kernel + initrd + cmdline) — no UEFI required
- Virtio MMIO device stack
  - Block (virtio-blk) — raw disk image, supports read/write/flush/get-id
  - Console (virtio-console) — secondary serial target
  - Network (virtio-net) — NAT userspace stack with DHCP, DNS, ICMP, TCP, UDP
  - GPU (virtio-gpu-2d) — 2D framebuffer with frame callback
  - Input (virtio-input) — keyboard and tablet (absolute pointer)
- PL011 UART as the primary console, wired through IPC to the host
- PL031 RTC
- GICv3 interrupt controller via `hv_gic_create`
- PSCI 1.0 (CPU_ON, CPU_OFF, SYSTEM_OFF, SYSTEM_RESET, CPU_SUSPEND, FEATURES)
- Multi-vCPU support (up to 8)
- Flattened Device Tree (DTB) generated at runtime
- Port forwarding (host → guest TCP)
- IPC-based serial channels (one per serial port)
- JSON configuration API

---

## Requirements

- Apple Silicon Mac (M1 or later)
- macOS 13 or later
- Xcode 15+ / Clang with C++23
- `Hypervisor.framework` entitlement (`com.apple.security.hypervisor`)
- `Virtualization.framework` entitlement (VZ backend only)

---

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.logicalcpu)
```

The CLI tool is built at `build/cli/vm-cli`.

---

## Quick Start

```bash
vm-cli run \
  --engine=hvf \
  --name=debian \
  --cpu=2 \
  --ram=2GB \
  --image=/path/to/disk.img \
  --kernel=/path/to/vmlinuz \
  --initrd=/path/to/initrd \
  --cmdline="console=ttyAMA0,115200 earlycon=pl011,0x9000000 root=/dev/vda1 ro" \
  --serial=1 \
  --network
```

With port forwarding:

```bash
vm-cli run \
  --engine=hvf \
  --name=debian \
  --cpu=4 \
  --ram=4GB \
  --image=/path/to/disk.img \
  --kernel=/path/to/vmlinuz \
  --initrd=/path/to/initrd \
  --serial=1 \
  --network \
  --forward=2222:22 \
  --forward=8080:80
```

Then SSH in from the host:

```bash
ssh -p 2222 user@localhost
```

---

## CLI Reference

| Flag | Description | Default |
|---|---|---|
| `--engine=<vz\|hvf>` | Virtualization backend | `vz` |
| `--name=<name>` | VM name (used for IPC channel names) | `vm` |
| `--cpu=<n>` | vCPU count | `1` |
| `--ram=<size>` | RAM size (`MB` or `GB` suffix) | `2GB` |
| `--image=<path>` | Raw disk image | required |
| `--kernel=<path>` | Uncompressed arm64 `Image` | — |
| `--initrd=<path>` | Initial ramdisk | — |
| `--cmdline=<args>` | Kernel command line | see below |
| `--serial=<n>` | Number of serial ports to expose | `0` |
| `--display=<WxH>` | Display resolution (e.g. `1920x1080`) | — |
| `--network` | Enable NAT networking (virtio-net, `10.0.2.x`) | disabled |
| `--forward=H:G` | Forward host port H to guest port G (repeatable) | — |

Default kernel command line when `--kernel` is given and `--cmdline` is omitted:

```
console=ttyAMA0,115200 earlycon=pl011,0x9000000 root=/dev/vda1 ro
```

---

## Library API

```cpp
#include <compute/vm.hpp>

// Create a VM from a JSON config string. Returns a handle.
compute::vm::Handle h = compute::vm::Create(config_json);

// Attach a native UI surface (Metal layer, etc.)
compute::vm::UI(h, native_handle);

// Stop and destroy the VM.
compute::vm::Destroy(h);
```

### JSON Config Schema

```json
{
  "engine":    "hvf",
  "name":      "my-vm",
  "cpu_count": 2,
  "ram_size":  "4GB",
  "image":     "/path/to/disk.img",
  "kernel":    "/path/to/vmlinuz",
  "initrd":    "/path/to/initrd",
  "cmdline":   "console=ttyAMA0,115200 root=/dev/vda1 ro",
  "network": {
    "enabled": true,
    "port_forwards": [
      { "host": 2222, "guest": 22 },
      { "host": 8080, "guest": 80 }
    ]
  },
  "serial": [
    { "port": 0, "channel": "my-vm-serial-0" }
  ],
  "display": {
    "width":  1920,
    "height": 1080
  }
}
```

---

## Guest Physical Address Map

| Region | Base GPA | Size |
|---|---|---|
| RAM | `0x40000000` | configured |
| DTB load | `0x40000000` | — |
| Kernel load | `0x40400000` | — |
| Initrd load | `0x48000000` | — |
| PL011 UART | `0x09000000` | 4 KiB |
| PL031 RTC | `0x09010000` | 4 KiB |
| GIC distributor | `0x08000000` | 64 KiB |
| GIC redistributor | `0x080A0000` | 128 KiB × vCPU count |
| virtio-console | `0x0A000000` | 512 B |
| virtio-blk | `0x0A000200` | 512 B |
| virtio-net | `0x0A000400` | 512 B |
| virtio-gpu | `0x0A000600` | 512 B |
| virtio-keyboard | `0x0A000800` | 512 B |
| virtio-tablet | `0x0A000A00` | 512 B |

---

## Network Stack (HVF)

When `--network` is passed, a userspace NAT stack is started. The guest is
offered a static lease via DHCP:

| Role | Address |
|---|---|
| Guest | `10.0.2.15` |
| Gateway / DNS proxy | `10.0.2.2` |
| Upstream DNS | `8.8.8.8` (forwarded) |

Supported protocols:

- **ARP** — gateway and DNS IP resolve to the gateway MAC
- **ICMP** — echo requests to the gateway are answered
- **DHCP** — DISCOVER and REQUEST handled; 24-hour lease
- **DNS** — UDP port 53 forwarded to `8.8.8.8` and relayed back
- **UDP** — generic passthrough with 2-second reply window
- **TCP** — full NAT with per-connection state machine and IO thread;
  connections to any routable host IP are supported

Inbound port forwards are listened on `127.0.0.1:<host_port>` and injected
into the guest via a synthetic TCP handshake from the gateway address.

---

## Architecture

```
┌─────────────────────────────────────────┐
│              compute::vm API            │  vm.cpp
└────────────┬────────────────┬───────────┘
             │                │
    ┌────────▼──────┐  ┌──────▼────────┐
    │  HvfBackend   │  │   VzBackend   │
    └────────┬──────┘  └───────────────┘
             │
    ┌────────▼──────────────────────────────┐
    │              VMContext                 │
    │  ┌────────┐ ┌───────┐ ┌────────────┐  │
    │  │  VCpu  │ │Memory │ │  Pl011State│  │
    │  └───┬────┘ └───────┘ └────────────┘  │
    │      │  ExitHandler                   │
    │      │  ├─ WFI/WFE → sched_yield      │
    │      │  ├─ MSR/MRS → zero             │
    │      │  ├─ HVC/SMC → PSCI dispatch    │
    │      │  └─ DABT/IABT → MMIO dispatch  │
    │      │       ├─ PL011 UART            │
    │      │       ├─ PL031 RTC             │
    │      │       ├─ GICv3 dist/redist     │
    │      │       └─ virtio-mmio devices   │
    │      │            ├─ Console          │
    │      │            ├─ Block            │
    │      │            ├─ Net ──► Stack    │
    │      │            ├─ GPU              │
    │      │            ├─ Keyboard         │
    │      │            └─ Tablet           │
    └──────────────────────────────────────┘
```

---

## Disk Images

Any standard ARM64 cloud image works. Debian is recommended for its minimal
cloud images:

```bash
# Download Debian 12 arm64 cloud image
curl -LO https://cloud.debian.org/images/cloud/bookworm/latest/debian-12-nocloud-arm64.raw
```

The kernel and initrd can be extracted from the image or downloaded separately
from the same Debian mirror. The image must use `/dev/vda1` as the root
partition (standard for all major cloud images).

---

## Limitations

- UEFI boot is not supported in the HVF backend; direct Linux boot only
- The VZ backend exposes the `Virtualization.framework` API and supports UEFI
  via macOS-provided firmware
- virtio-gpu is 2D only; no 3D / virgl support
- Network stack runs entirely in userspace; raw sockets and bridged networking
  are not supported
- `--display` wires up the GPU frame callback but Metal rendering must be
  connected by the embedding application via `compute::vm::UI()`

---

## License

MIT