#include <iostream>
#include <string>

#include "cm/protocol/version.h"
#include "cm/version.h"

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--version") {
        std::cout << "cloud-media-client " << cm::version()
                  << " (protocol v" << cm::protocol::version() << ")\n";
        return 0;
    }

    std::cout << "cloud-media-client " << cm::version() << "\n";
    std::cout << "Phase 1 placeholder: client skeleton is not implemented yet.\n";
    return 0;
}
