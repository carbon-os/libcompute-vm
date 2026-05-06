


libcompute-vm/
├── CMakeLists.txt
├── include/
│   └── compute/
│       └── vm.hpp                    # Public API (Handle, Create, Destroy, UI)
├── src/
│   ├── vm.cpp                        # Dispatcher: JSON -> VZ or HVF Backend
│   ├── backend.hpp                   # internal::Backend interface
│   │
│   ├── vz/                           # Apple Virtualization.framework Backend
│   │   ├── vz_backend.hpp
│   │   └── vz_backend.mm
│   │
│   ├── common/                       # Shared internal utilities (NEW)
│   │   ├── logger.hpp                
│   │   └── logger.cpp
│   │
│   └── hvf/                          # Custom Hypervisor.framework Backend (NEW)
│       ├── hvf_backend.hpp           # Implements internal::Backend (replaces VirtualMachine)
│       ├── hvf_backend.cpp           # Parses JSON, orchestrates VMContext
│       │
│       ├── vm_context.hpp / .cpp     # Central VMM state
│       ├── vcpu.hpp / .cpp           # Threading & Register state
│       ├── exit_handler.hpp / .cpp   # EC/ESR decoding, MMIO routing
│       ├── memory.hpp / .cpp         # Guest RAM & shm_open
│       │
│       ├── fw_dtb.hpp / .cpp         # Flattened Device Tree generator
│       ├── fw_gic.hpp / .cpp         # GICv3 interrupt raising
│       ├── fw_psci.hpp / .cpp        # CPU_ON, SYSTEM_OFF, etc.
│       ├── fw_loader.hpp / .cpp      # arm64 Image & initrd loading
│       │
│       ├── virtio_block.hpp / .cpp   # Raw disk image MMIO
│       ├── virtio_console.hpp / .cpp # Serial console (Needs IPC refactor)
│       ├── virtio_gpu.hpp / .cpp     # 2D Framebuffer
│       ├── virtio_input.hpp / .cpp   # Keyboard & Tablet
│       ├── virtio_net.hpp / .cpp     # Virtio Network ring
│       │
│       └──network.hpp / .cpp        # Userspace NAT & Port forwarding