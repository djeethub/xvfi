#include <iostream>
#include <unordered_set>
#include <filesystem>

#include "appstate.hpp"

namespace fs = std::filesystem;

AppState state;
int idx_in = -1;
int idx_out = -1;

bool parse_args(int argc, char** argv) {
    if (argc < 3)
        return false;

    for (auto i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (i + 1 >= argc)
                return false;
            state.encoder.args.emplace_back(argv[i] + 1, argv[i + 1]);
            i++;
        } else {
            if (idx_in < 0)
                idx_in = i;
            else if (idx_out < 0)
                idx_out = i;
            else
                return false;
        }
    }

    return idx_in > 0 && idx_out > 0;
}

int main(int argc, char** argv) {
    try {
        if (!parse_args(argc, argv)) {
            std::cerr << "Usage: " << argv[0] << " [encoder args] <in_dir> <out_dir>\n";
            return 1;
        }

        fs::path in_dir{argv[idx_in]};
        fs::path out_dir{argv[idx_out]};

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
            if (!state.open((in_dir / entry).string(), (out_dir / entry).string())) {
                std::cerr << "Failed to open " << entry << ".\n";
                continue;
            }
            auto start = std::chrono::steady_clock::now();
            if (!state.process()) {
                std::cerr << "Failed to process " << entry << ".\n";
            }
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            std::cout << "Time: " << duration.count() << " ms" << std::endl;
        }
        std::cout << "All done.\n";

    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
