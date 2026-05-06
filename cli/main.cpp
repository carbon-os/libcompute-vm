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
};

// ── usage ─────────────────────────────────────────────────────────────────────

static void usage(const char* prog) {
    std::cerr
        << "\nusage: " << prog << " run [options]\n"
        << "\n"
        << "options:\n"
        << "  --name=<name>      VM name        (default: vm)\n"
        << "  --cpu=<n>          vCPU count     (default: 1)\n"
        << "  --ram=<size>       RAM size       (default: 2GB)\n"
        << "  --disk=<size>      Disk size      (default: 20GB)\n"
        << "  --image=<path>     Boot image     (required)\n"
        << "  --display=<WxH>    Resolution     (e.g. 1920x1080)\n"
        << "  --serial=<n>       Serial ports   (default: 0, logs hvc0..hvcN-1 to stderr)\n"
        << "\n"
        << "the vm is destroyed automatically on exit (ctrl-c or normal close)\n"
        << "\n"
        << "example:\n"
        << "  vm-cli run --name=ubuntu --cpu=4 --ram=8GB --image=/images/ubuntu.img --serial=1\n";
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

    const std::string vm_name   = Args::get(argc, argv, "name", "vm");
    const int         serial_n  = std::stoi(Args::get(argc, argv, "serial", "0"));

    nlohmann::json config;
    config["name"]      = vm_name;
    config["cpu_count"] = std::stoi(Args::get(argc, argv, "cpu",  "1"));
    config["ram_size"]  = Args::get(argc, argv, "ram",  "2GB");
    config["disk_size"] = Args::get(argc, argv, "disk", "20GB");
    config["image"]     = image;

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

    // ── serial config ─────────────────────────────────────────
    // channel names are <vm_name>-serial-<port>
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

        std::cerr << "vm running (handle " << handle << ") — ctrl-c to stop\n";

        // ── connect serial clients ─────────────────────────────
        // Brief wait for IPC server sockets to be created by the backend threads
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
                // raw kernel output — write directly to stderr as-is
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