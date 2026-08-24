#include "oaa_core.h"
#include <iostream>
#include <iomanip>

using namespace oaa;

void print_help() {
    std::cout << "\033[1;32moaa-cli (C++ OAA Engine v1.0.0)\033[0m\n"
              << "Usage: oaa-cli <command> [arguments...]\n\n"
              << "Commands:\n"
              << "  new <dir> [name] [version]   Initialize a new package directory\n"
              << "  build <src_dir> [out.oaa]    Build .oaa package\n"
              << "  inspect <file.oaa>           Inspect metadata from .oaa archive\n"
              << "  extract <file.oaa> <dest>    Extract .oaa archive to destination\n"
              << "  list <file.oaa>              List files in .oaa archive\n"
              << "  verify <file.oaa>            Verify integrity of .oaa archive\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "new" && argc >= 3) {
        std::string dir = argv[2];
        OaaMeta meta;
        meta.name = (argc >= 4) ? argv[3] : "app-demo";
        meta.version = (argc >= 5) ? argv[4] : "1.0.0";
        meta.description = "Built with oaa-cli";
        if (OaaCore::init_skeleton(dir, meta)) {
            std::cout << ":: Initialized OAA skeleton at: " << dir << "\n";
            return 0;
        }
        std::cerr << "Error initializing directory.\n";
        return 1;
    } else if (cmd == "build" && argc >= 3) {
        std::string src = argv[2];
        std::string out = (argc >= 4) ? argv[3] : "";
        auto res = OaaCore::build_package(src, out);
        if (res) {
            std::cout << ":: Package built: " << *res << "\n";
            return 0;
        }
        std::cerr << "Error building package.\n";
        return 1;
    } else if (cmd == "inspect" && argc >= 3) {
        auto meta = OaaCore::inspect_package(argv[2]);
        if (!meta) {
            std::cerr << "Error inspecting package: " << argv[2] << "\n";
            return 1;
        }
        std::cout << "Artifact:     " << argv[2] << "\n"
                  << "Name:         " << meta->name << "\n"
                  << "Version:      " << meta->version << "\n"
                  << "Namespace:    " << meta->ns << "\n"
                  << "Arch:         " << meta->architecture << "\n"
                  << "Description:  " << meta->description << "\n";
        if (!meta->dependencies.empty()) {
            std::cout << "Dependencies: ";
            for (const auto& d : meta->dependencies) std::cout << d << " ";
            std::cout << "\n";
        }
        return 0;
    } else if (cmd == "extract" && argc >= 4) {
        if (OaaCore::extract_package(argv[2], argv[3])) {
            std::cout << ":: Extracted " << argv[2] << " to " << argv[3] << "\n";
            return 0;
        }
        std::cerr << "Error extracting package.\n";
        return 1;
    } else if (cmd == "list" && argc >= 3) {
        auto files = OaaCore::list_contents(argv[2]);
        for (const auto& f : files) std::cout << f << "\n";
        return 0;
    } else if (cmd == "verify" && argc >= 3) {
        if (OaaCore::verify_package(argv[2])) {
            std::cout << ":: " << argv[2] << " is VALID.\n";
            return 0;
        }
        std::cerr << ":: " << argv[2] << " is CORRUPTED.\n";
        return 1;
    }

    print_help();
    return 1;
}
