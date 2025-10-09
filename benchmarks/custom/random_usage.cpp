#include "benchmark_util.hpp"

#include "compio.h"

#include "sample_data.hpp"

int main() {
    compio_config config;
    compio_archive *archive;
    compio_file *file;

    std::string fn = get_temporary_filename();

    compio_build_default_config(&config);
    config.cache_size__blocks = 32;
    config.cache_size__compression = 32;

    std::size_t file_size = 1 << 18;
    std::size_t n_operations = 1 << 12;
    std::size_t n_repetitions = 1;

    std::vector<unsigned char> buffer(file_size);

    std::minstd_rand rng;
    std::uniform_int_distribution<int> d_op(0, 3);
    std::uniform_int_distribution<int> d_pos(0, file_size - 2);

    for (int k = 0; k < n_repetitions; ++k) {
        rng.seed(k);

        int cursor = 0;
        int current_fsize = 0;

        archive = compio_open_archive(fn.c_str(), "w+", &config);
        file = compio_open_file("A", archive);

        for (int i = 0; i < n_operations; ++i) {
            std::size_t max_size = std::min<uint64_t>(file_size - cursor, sizeof(html_data));
            std::uniform_int_distribution<int> d_size(1, max_size);
            int size = d_size(rng);

            switch (d_op(rng)) {
            case 0: {
                cursor = d_pos(rng);
                compio_seek(file, cursor, COMP_SEEK_SET);
                break;
            }
            case 1: {
                compio_tell(file);
                break;
            }
            case 2: {
                if (cursor < current_fsize) {
                    compio_read(buffer.data(), size, file);
                    cursor += size;
                    break;
                }
            }
            case 3: {
                if (cursor < file_size) {
                    std::uniform_int_distribution<int> d_start(0, sizeof(html_data) - size);
                    int start = d_start(rng);

                    compio_write(html_data + start, size, file);
                    cursor += size;
                    current_fsize = std::max(current_fsize, cursor);
                    break;
                }
            }
            }
        }

        compio_close_file(file);
        compio_close_archive(archive);
    }

    remove(fn.c_str());

    return 0;
}