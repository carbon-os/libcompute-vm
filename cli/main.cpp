#include "compute/vm.hpp"

#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <ipc/ipc.hpp>

// ── signal handling ───────────────────────────────────────────────────────────

static std::atomic<compute::vm::Handle> g_handle{0};

static void on_signal(int) {
    const auto handle = g_handle.load();
    if (handle) {
        std::cerr << "\nstopping vm...\n";
        compute::vm::Destroy(handle);
    }
    std::exit(0);
}

// ── tiny arg parser ───────────────────────────────────────────────────────────

struct Args {
    static std::string get(int argc, char* argv[], std::string_view key,
                           std::string_view fallback = "") {
        const std::string flag  = "--" + std::string(key) + "=";
        const std::string flag2 = "--" + std::string(key);
        for (int i = 0; i < argc; ++i) {
            std::string_view a = argv[i];
            if (a.starts_with(flag))
                return std::string(a.substr(flag.size()));
            if (a == flag2 && i + 1 < argc)
                return argv[i + 1];
        }
        return std::string(fallback);
    }

    // Returns all values for a repeated flag, e.g. --forward=8080:80 --forward=2222:22
    static std::vector<std::string> get_all(int argc, char* argv[], std::string_view key) {
        const std::string flag = "--" + std::string(key) + "=";
        std::vector<std::string> results;
        for (int i = 0; i < argc; ++i) {
            std::string_view a = argv[i];
            if (a.starts_with(flag))
                results.emplace_back(a.substr(flag.size()));
        }
        return results;
    }

    static bool flag(int argc, char* argv[], std::string_view key) {
        const std::string v = get(argc, argv, key);
        return v == "true" || v == "1" || v == "on" || v == "";
        // treat bare --network (empty value from flag2 path) as true
    }

    // Returns true if the bare flag (no value) is present, e.g. --network
    static bool present(int argc, char* argv[], std::string_view key) {
        const std::string flag2 = "--" + std::string(key);
        const std::string flagp = "--" + std::string(key) + "=";
        for (int i = 0; i < argc; ++i) {
            std::string_view a = argv[i];
            if (a == flag2 || a.starts_with(flagp))
                return true;
        }
        return false;
    }
};

// ── usage ─────────────────────────────────────────────────────────────────────

static void usage(const char* prog) {
    std::cerr
        << "\nusage: " << prog << " run [options]\n"
        << "\n"
        << "options:\n"
        << "  --engine=<vz|hvf>  Backend engine           (default: vz)\n"
        << "  --name=<name>      VM name                  (default: vm)\n"
        << "  --cpu=<n>          vCPU count               (default: 1)\n"
        << "  --ram=<size>       RAM size                 (default: 2GB)\n"
        << "  --disk=<size>      Disk size                (default: 20GB)\n"
        << "  --image=<path>     Boot image               (required)\n"
        << "  --kernel=<path>    Direct Linux boot: path to vmlinuz\n"
        << "  --initrd=<path>    Direct Linux boot: path to initramfs\n"
        << "  --cmdline=<args>   Direct Linux boot: kernel arguments\n"
        << "  --display=<WxH>    Resolution               (e.g. 1920x1080)\n"
        << "  --serial=<n>       Serial ports             (default: 0)\n"
        << "  --network          Enable NAT networking (virtio-net, 10.0.2.x)\n"
        << "  --forward=H:G      Forward host port H to guest port G\n"
        << "                     (requires --network, repeatable)\n"
        << "\n"
        << "the vm is destroyed automatically on exit (ctrl-c or normal close)\n"
        << "\n"
        << "examples:\n"
        << "  vm-cli run --engine=hvf --name=ubuntu --cpu=4 --ram=8GB \\\n"
        << "             --image=/images/ubuntu.img --kernel=vmlinuz --serial=1\n"
        << "\n"
        << "  vm-cli run --engine=hvf --name=debian --cpu=2 --ram=2GB \\\n"
        << "             --image=/images/debian.img --kernel=vmlinuz \\\n"
        << "             --network --forward=2222:22 --forward=8080:80\n";
}

// ── port-forward parser ───────────────────────────────────────────────────────

struct PortForward { uint16_t host_port, guest_port; };

static std::optional<PortForward> parse_forward(const std::string& s) {
    const auto colon = s.find(':');
    if (colon == std::string::npos) {
        std::cerr << "warning: ignoring malformed --forward=" << s
                  << " (expected H:G)\n";
        return std::nullopt;
    }
    try {
        return PortForward{
            static_cast<uint16_t>(std::stoi(s.substr(0, colon))),
            static_cast<uint16_t>(std::stoi(s.substr(colon + 1)))
        };
    } catch (...) {
        std::cerr << "warning: ignoring malformed --forward=" << s << "\n";
        return std::nullopt;
    }
}

// ── entry point ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2 || std::string_view(argv[1]) != "run") {
        usage(argv[0]);
        return 1;
    }

    argc -= 1;
    argv += 1;

    const std::string image = Args::get(argc, argv, "image");
    if (image.empty()) {
        std::cerr << "error: --image is required\n";
        return 1;
    }

    const std::string vm_name  = Args::get(argc, argv, "name",   "vm");
    const std::string engine   = Args::get(argc, argv, "engine", "vz");
    const int         serial_n = std::stoi(Args::get(argc, argv, "serial", "0"));

    nlohmann::json config;
    config["engine"]    = engine;
    config["name"]      = vm_name;
    config["cpu_count"] = std::stoi(Args::get(argc, argv, "cpu",  "1"));
    config["ram_size"]  = Args::get(argc, argv, "ram",  "2GB");
    config["disk_size"] = Args::get(argc, argv, "disk", "20GB");
    config["image"]     = image;

    // ── direct linux boot ─────────────────────────────────────
    const std::string kernel = Args::get(argc, argv, "kernel");
    if (!kernel.empty()) {
        config["kernel"] = kernel;

        const std::string initrd = Args::get(argc, argv, "initrd");
        if (!initrd.empty()) config["initrd"] = initrd;

        config["cmdline"] = Args::get(argc, argv, "cmdline",
            "console=ttyAMA0,115200 earlycon=pl011,0x9000000 root=/dev/vda1 ro");
    }

    // ── display ───────────────────────────────────────────────
    const std::string display = Args::get(argc, argv, "display");
    if (!display.empty()) {
        const auto x = display.find('x');
        if (x == std::string::npos) {
            std::cerr << "error: --display must be WxH (e.g. 1920x1080)\n";
            return 1;
        }
        config["display"]["width"]  = std::stoi(display.substr(0, x));
        config["display"]["height"] = std::stoi(display.substr(x + 1));
    }

    // ── network ───────────────────────────────────────────────
    if (Args::present(argc, argv, "network")) {
        config["network"]["enabled"] = true;

        const auto forwards = Args::get_all(argc, argv, "forward");
        if (!forwards.empty()) {
            config["network"]["port_forwards"] = nlohmann::json::array();
            for (const auto& s : forwards) {
                if (auto pf = parse_forward(s)) {
                    config["network"]["port_forwards"].push_back({
                        {"host",  pf->host_port},
                        {"guest", pf->guest_port}
                    });
                    std::cerr << "[net] port forward host:" << pf->host_port
                              << " -> guest:" << pf->guest_port << "\n";
                }
            }
        }
    }

    // ── serial config ─────────────────────────────────────────
    std::vector<std::string> serial_channels;
    if (serial_n > 0) {
        config["serial"] = nlohmann::json::array();
        for (int i = 0; i < serial_n; ++i) {
            std::string ch = vm_name + "-serial-" + std::to_string(i);
            config["serial"].push_back({ {"port", i}, {"channel", ch} });
            serial_channels.push_back(std::move(ch));
        }
    }

    try {
        const auto handle = compute::vm::Create(config.dump().c_str());
        g_handle.store(handle);

        std::signal(SIGINT,  on_signal);
        std::signal(SIGTERM, on_signal);

        std::cerr << "vm running (handle " << handle << ", engine "
                  << engine << ") — ctrl-c to stop\n";

        // ── connect serial clients ─────────────────────────────
        if (!serial_channels.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::vector<std::unique_ptr<ipc::Client>> clients;
        for (int i = 0; i < static_cast<int>(serial_channels.size()); ++i) {
            auto client = std::make_unique<ipc::Client>(serial_channels[i]);

            client->on_connect([i] {
                std::cerr << "[serial:" << i << "] connected\n";
            });
            client->on_disconnect([i] {
                std::cerr << "[serial:" << i << "] disconnected\n";
            });
            client->on_error([i](ipc::Error err) {
                std::cerr << "[serial:" << i << "] error: " << err.message << "\n";
            });
            client->on_message([](ipc::Message msg) {
                auto data = msg.binary();
                ::write(STDERR_FILENO, data.data(), data.size());
            });

            client->connect();
            clients.push_back(std::move(client));
        }

        while (true) pause();

    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}