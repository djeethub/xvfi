#include <iostream>
#include <unordered_set>
#include <filesystem>

#include "appstate.hpp"

namespace fs = std::filesystem;

AppState state;

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0] << " <in_dir> <out_dir>\n";
            return 1;
        }

        fs::path in_dir{argv[1]};
        fs::path out_dir{argv[2]};

        std::unordered_set<std::string> files;
//        files.reserve(10000); // Adjust based on expected scale

        for (const auto& entry : fs::directory_iterator(in_dir)) {
            if (entry.is_regular_file()) {
                files.insert(entry.path().filename().string());
            }
        }
        for (const auto& entry : fs::directory_iterator(out_dir)) {
            if (entry.is_regular_file()) {
                files.erase(entry.path().filename().string());
            }
        }

        std::cout << "Processing " << files.size() << " files.\n";
        int n = 0;
        for (const auto& entry : files) {
            std::cout << ++n << "/" << files.size() << ": " << entry << std::endl;
            if (!state.open(in_dir / entry, out_dir / entry)) {
                std::cerr << "Failed to open " << entry << ".\n";
                continue;
            }
            if (!state.process()) {
                std::cerr << "Failed to process " << entry << ".\n";
            }
        }
        std::cout << "All done.\n";

    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
