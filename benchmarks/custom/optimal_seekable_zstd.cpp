#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "benchmark_util.hpp"
#include "optimal_benchmark_constants.hpp"
#include "zstd_seekable.h"

static void write_seekable_zstd(const std::string &path, const char *data, std::size_t data_size,
                                unsigned max_frame_size) {
    FILE *out = fopen(path.c_str(), "wb");
    if (!out)
        throw std::runtime_error("fopen failed");
    ZSTD_seekable_CStream *stream = ZSTD_seekable_createCStream();
    if (!stream) {
        fclose(out);
        throw std::runtime_error("ZSTD_seekable_createCStream failed");
    }
    size_t ret = ZSTD_seekable_initCStream(stream, 1, 1, max_frame_size);
    if (ZSTD_isError(ret)) {
        ZSTD_seekable_freeCStream(stream);
        fclose(out);
        throw std::runtime_error("ZSTD_seekable_initCStream failed");
    }
    std::vector<char> outbuf(ZSTD_CStreamOutSize());
    const std::size_t CHUNK = 1 << 15;
    std::size_t pos = 0;
    while (pos < data_size) {
        std::size_t chunk = std::min(CHUNK, data_size - pos);
        ZSTD_inBuffer in{data + pos, chunk, 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer outb{outbuf.data(), outbuf.size(), 0};
            ret = ZSTD_seekable_compressStream(stream, &outb, &in);
            if (ZSTD_isError(ret)) {
                ZSTD_seekable_freeCStream(stream);
                fclose(out);
                throw std::runtime_error("compressStream failed");
            }
            fwrite(outbuf.data(), 1, outb.pos, out);
        }
        pos += chunk;
    }
    do {
        ZSTD_outBuffer outb{outbuf.data(), outbuf.size(), 0};
        ret = ZSTD_seekable_endStream(stream, &outb);
        if (ZSTD_isError(ret)) {
            ZSTD_seekable_freeCStream(stream);
            fclose(out);
            throw std::runtime_error("endStream failed");
        }
        fwrite(outbuf.data(), 1, outb.pos, out);
    } while (ret != 0);
    ZSTD_seekable_freeCStream(stream);
    fclose(out);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <max_frame_size>\n";
        return 1;
    }

    unsigned max_frame_size = std::stoul(argv[1]);

    std::minstd_rand rng;
    std::unique_ptr<char[]> buffer(new char[FILE_SIZE]);
    std::cout << "file_offset,throughput,file_size\n";

    for (outer_loop.reset(); !outer_loop.done(); ++outer_loop.count) {
        auto [sample_data, file_offset] = load_random_sample_data(rng);

        UsageStrategy strategy(0, sample_data.data(), sample_data.size(), FILE_SIZE, GAMMA_SHAPE,
                               GAMMA_SCALE, REGION_SIZE, N_SWITCH);

        std::string file_path = get_temporary_filename() + ".seek.zst";
        write_seekable_zstd(file_path, sample_data.data(), FILE_SIZE, max_frame_size);
        unsigned long file_size_stored = get_file_size(file_path.c_str());

        for (inner_loop.reset(); !inner_loop.done(); ++inner_loop.count) {
            FILE *file = fopen(file_path.c_str(), "rb");
            if (!file) {
                std::cerr << "fopen failed\n";
                break;
            }
            ZSTD_seekable *seek = ZSTD_seekable_create();
            if (!seek || ZSTD_isError(ZSTD_seekable_initFile(seek, file))) {
                if (seek)
                    ZSTD_seekable_free(seek);
                fclose(file);
                std::cerr << "ZSTD_seekable init failed\n";
                break;
            }

            Timer iter_timer;
            bool failed = false;
            std::size_t iter_bytes = 0;
            for (std::size_t i = 0; i < N_OPERATIONS; ++i) {
                auto op = strategy.get_op();
                size_t n = ZSTD_seekable_decompress(seek, buffer.get(), op.size, op.pos);
                if (ZSTD_isError(n) || n != op.size) {
                    failed = true;
                    break;
                }
                iter_bytes += op.size;
            }

            ZSTD_seekable_free(seek);
            fclose(file);

            if (failed)
                break;

            double elapsed = iter_timer.elapsed_seconds();
            double tp = static_cast<double>(iter_bytes) / elapsed;

            std::cout << file_offset << "," << tp << "," << file_size_stored << "\n";
        }

        remove(file_path.c_str());
    }

    return 0;
}
