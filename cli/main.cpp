#include "compute/vm.hpp"

#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <atomic>
#include <unistd.h>
#include <nlohmann/json.hpp>

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
        << "\n"
        << "the vm is destroyed automatically on exit (ctrl-c or normal close)\n"
        << "\n"
        << "example:\n"
        << "  vm-cli run --name=ubuntu --cpu=4 --ram=8GB --image=/images/ubuntu.img --display=1920x1080\n";
}

// ── entry point ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2 || std::string_view(argv[1]) != "run") {
        usage(argv[0]);
        return 1;
    }

    // shift past sub-command
    argc -= 1;
    argv += 1;

    const std::string image = Args::get(argc, argv, "image");
    if (image.empty()) {
        std::cerr << "error: --image is required\n";
        return 1;
    }

    nlohmann::json config;
    config["name"]      = Args::get(argc, argv, "name",  "vm");
    config["cpu_count"] = std::stoi(Args::get(argc, argv, "cpu",  "1"));
    config["ram_size"]  = Args::get(argc, argv, "ram",   "2GB");
    config["disk_size"] = Args::get(argc, argv, "disk",  "20GB");
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

    try {
        const auto handle = compute::vm::Create(config.dump().c_str());
        g_handle.store(handle);

        std::signal(SIGINT,  on_signal);
        std::signal(SIGTERM, on_signal);

        std::cerr << "vm running (handle " << handle << ") — ctrl-c to stop\n";

        while (true) pause();

    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}