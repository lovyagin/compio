#include <iostream>
#include "compio.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <archive_path> <output_dir>" << std::endl;
        return 1;
    }
    int result = compio_repair(argv[1], argv[2]);
    if (result < 0) {
        std::cerr << "Repair failed." << std::endl;
        return 1;
    }
    std::cout << "Recovered " << result << " files." << std::endl;
    return 0;
}
