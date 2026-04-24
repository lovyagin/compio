#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

#include "compio.h"
#include "sample_data.hpp"
#include "zstd_seekable.h"

extern "C" {
#include "zran.h"
}

namespace {

struct AccessOp {
    std::size_t pos;
    std::size_t size;
};

class LocalityGenerator {
public:
    LocalityGenerator(int seed, std::size_t sample_data_size, std::size_t file_size,
                      double gamma_shape, double gamma_scale, std::size_t region_size,
                      std::size_t n_switch)
        : rng_(seed),
          file_size_(file_size),
          sample_data_size_(sample_data_size),
          gamma_dist_(gamma_shape, gamma_scale),
          region_size_(std::min(region_size, file_size)),
          region_start_dist_(0, file_size - region_size_),
          n_ops_until_switch_(n_switch),
          n_switch_(n_switch) {
        region_start_ = region_start_dist_(rng_);
    }

    AccessOp next() {
        if (--n_ops_until_switch_ == 0) {
            region_start_ = region_start_dist_(rng_);
            n_ops_until_switch_ = n_switch_;
        }

        const double gamma_sample = gamma_dist_(rng_);
        const std::size_t max_possible = std::min(region_size_, sample_data_size_);
        std::size_t size =
            static_cast<std::size_t>(std::min(gamma_sample, static_cast<double>(max_possible)));
        if (size == 0) {
            size = 1;
        }

        std::uniform_int_distribution<std::size_t> offset_dist(0, region_size_ - size);
        const std::size_t offset = offset_dist(rng_);
        return AccessOp{region_start_ + offset, size};
    }

private:
    std::minstd_rand rng_;
    std::size_t file_size_;
    std::size_t sample_data_size_;
    std::gamma_distribution<double> gamma_dist_;
    std::size_t region_size_;
    std::uniform_int_distribution<std::size_t> region_start_dist_;
    std::size_t region_start_;
    std::size_t n_ops_until_switch_;
    std::size_t n_switch_;
};

std::string make_temp_path(const char* suffix) {
    static std::minstd_rand rng(0);
    static std::uniform_int_distribution<int> dist(10000, 99999);
    std::string path;
    do {
        path = "locality_cmp_" + std::to_string(dist(rng)) + suffix;
    } while (std::filesystem::exists(path));
    return path;
}

void remove_if_exists(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

std::vector<char> build_payload(std::size_t file_size) {
    const std::size_t block_size = 4096;
    if (sizeof(html_data) <= block_size) {
        throw std::runtime_error("sample_data is too small for payload generation");
    }

    std::vector<char> payload(file_size);
    std::minstd_rand rng(0);
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(html_data) - block_size);

    for (std::size_t i = 0; i < file_size; i += block_size) {
        const std::size_t bytes_to_copy = std::min(block_size, file_size - i);
        std::copy_n(html_data + dist(rng), bytes_to_copy, payload.data() + i);
    }
    return payload;
}

std::vector<AccessOp> build_ops(std::size_t seed, std::size_t n_operations, std::size_t file_size,
                                double gamma_shape, double gamma_scale, std::size_t region_size,
                                std::size_t n_switch) {
    LocalityGenerator gen(static_cast<int>(seed), sizeof(html_data), file_size, gamma_shape,
                          gamma_scale, region_size, n_switch);
    std::vector<AccessOp> ops;
    ops.reserve(n_operations);
    for (std::size_t i = 0; i < n_operations; ++i) {
        ops.push_back(gen.next());
    }
    return ops;
}

compio_config make_compio_config() {
    compio_config config;
    compio_build_default_config(&config);
    config.wal_sync_mode = COMPIO_WAL_SYNC_NORMAL;
    return config;
}

void write_compio_archive(const std::string& archive_path, const std::vector<char>& payload) {
    compio_config config = make_compio_config();
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "w+", &config);
    if (!archive) {
        throw std::runtime_error("compio_open_archive failed for " + archive_path);
    }

    compio_file* file = compio_open_file("A", archive);
    if (!file) {
        compio_close_archive(archive);
        throw std::runtime_error("compio_open_file failed");
    }

    std::size_t pos = 0;
    constexpr std::size_t kChunk = 1u << 16;
    while (pos < payload.size()) {
        const std::size_t chunk = std::min(kChunk, payload.size() - pos);
        const uint64_t written = compio_write(payload.data() + pos, chunk, file);
        if (written != chunk) {
            compio_close_file(file);
            compio_close_archive(archive);
            throw std::runtime_error("compio_write failed while preparing payload");
        }
        pos += chunk;
    }

    compio_close_file(file);
    compio_close_archive(archive);
}

void write_gzip_file(const std::string& path, const std::vector<char>& payload,
                     std::size_t flush_interval) {
    gzFile gz = gzopen(path.c_str(), "wb");
    if (!gz) {
        throw std::runtime_error("gzopen failed for " + path);
    }

    std::size_t pos = 0;
    constexpr std::size_t kChunk = 1u << 15;
    std::size_t bytes_since_flush = 0;
    while (pos < payload.size()) {
        const std::size_t chunk = std::min(kChunk, payload.size() - pos);
        const int written = gzwrite(gz, payload.data() + pos, static_cast<unsigned>(chunk));
        if (written != static_cast<int>(chunk)) {
            int err_no = Z_OK;
            const char* err = gzerror(gz, &err_no);
            gzclose(gz);
            throw std::runtime_error(std::string("gzwrite failed: ") + (err ? err : "unknown"));
        }
        pos += chunk;
        bytes_since_flush += chunk;

        if (bytes_since_flush >= flush_interval) {
            const int flush_ret = gzflush(gz, Z_FULL_FLUSH);
            if (flush_ret != Z_OK) {
                int err_no = Z_OK;
                const char* err = gzerror(gz, &err_no);
                gzclose(gz);
                throw std::runtime_error(std::string("gzflush failed: ") +
                                         (err ? err : "unknown"));
            }
            bytes_since_flush = 0;
        }
    }

    if (gzclose(gz) != Z_OK) {
        throw std::runtime_error("gzclose failed");
    }
}

void write_seekable_zstd_file(const std::string& path, const std::vector<char>& payload,
                              unsigned max_frame_size) {
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) {
        throw std::runtime_error("fopen failed for " + path);
    }

    ZSTD_seekable_CStream* stream = ZSTD_seekable_createCStream();
    if (!stream) {
        std::fclose(out);
        throw std::runtime_error("ZSTD_seekable_createCStream failed");
    }

    size_t ret = ZSTD_seekable_initCStream(stream, /*compressionLevel=*/5, /*checksumFlag=*/1,
                                           max_frame_size);
    if (ZSTD_isError(ret)) {
        ZSTD_seekable_freeCStream(stream);
        std::fclose(out);
        throw std::runtime_error(std::string("ZSTD_seekable_initCStream failed: ") +
                                 ZSTD_getErrorName(ret));
    }

    std::vector<char> out_buffer(ZSTD_CStreamOutSize());
    std::size_t pos = 0;
    constexpr std::size_t kChunk = 1u << 15;
    while (pos < payload.size()) {
        const std::size_t chunk = std::min(kChunk, payload.size() - pos);
        ZSTD_inBuffer input{payload.data() + pos, chunk, 0};

        while (input.pos < input.size) {
            ZSTD_outBuffer output{out_buffer.data(), out_buffer.size(), 0};
            ret = ZSTD_seekable_compressStream(stream, &output, &input);
            if (ZSTD_isError(ret)) {
                ZSTD_seekable_freeCStream(stream);
                std::fclose(out);
                throw std::runtime_error(std::string("ZSTD_seekable_compressStream failed: ") +
                                         ZSTD_getErrorName(ret));
            }
            if (std::fwrite(out_buffer.data(), 1, output.pos, out) != output.pos) {
                ZSTD_seekable_freeCStream(stream);
                std::fclose(out);
                throw std::runtime_error("fwrite failed for seekable zstd payload");
            }
        }
        pos += chunk;
    }

    do {
        ZSTD_outBuffer output{out_buffer.data(), out_buffer.size(), 0};
        ret = ZSTD_seekable_endStream(stream, &output);
        if (ZSTD_isError(ret)) {
            ZSTD_seekable_freeCStream(stream);
            std::fclose(out);
            throw std::runtime_error(std::string("ZSTD_seekable_endStream failed: ") +
                                     ZSTD_getErrorName(ret));
        }
        if (std::fwrite(out_buffer.data(), 1, output.pos, out) != output.pos) {
            ZSTD_seekable_freeCStream(stream);
            std::fclose(out);
            throw std::runtime_error("fwrite failed while finalizing seekable zstd file");
        }
    } while (ret != 0);

    ZSTD_seekable_freeCStream(stream);
    std::fclose(out);
}

struct RunMetrics {
    std::size_t total_bytes = 0;
    double elapsed_sec = 0.0;
};

RunMetrics run_compio_reads(const std::string& archive_path, const std::vector<AccessOp>& ops,
                            std::vector<char>& scratch) {
    compio_config config = make_compio_config();
    compio_archive* archive = compio_open_archive(archive_path.c_str(), "r", &config);
    if (!archive) {
        throw std::runtime_error("compio_open_archive failed in read benchmark");
    }

    compio_file* file = compio_open_file("A", archive);
    if (!file) {
        compio_close_archive(archive);
        throw std::runtime_error("compio_open_file failed in read benchmark");
    }

    const auto begin = std::chrono::steady_clock::now();
    std::size_t total_bytes = 0;
    for (const auto& op : ops) {
        if (compio_seek(file, op.pos, COMPIO_SEEK_SET) != 0) {
            compio_close_file(file);
            compio_close_archive(archive);
            throw std::runtime_error("compio_seek failed");
        }
        const uint64_t read = compio_read(scratch.data(), op.size, file);
        if (read != op.size) {
            compio_close_file(file);
            compio_close_archive(archive);
            throw std::runtime_error("compio_read returned unexpected byte count");
        }
        total_bytes += op.size;
    }
    const auto end = std::chrono::steady_clock::now();

    compio_close_file(file);
    compio_close_archive(archive);

    return RunMetrics{
        total_bytes,
        std::chrono::duration<double>(end - begin).count(),
    };
}

RunMetrics run_zran_reads(FILE* file, struct deflate_index* index, const std::vector<AccessOp>& ops,
                          std::vector<char>& scratch) {
    const auto begin = std::chrono::steady_clock::now();
    std::size_t total_bytes = 0;
    for (const auto& op : ops) {
        const ptrdiff_t read = deflate_index_extract(
            file, index, static_cast<off_t>(op.pos), reinterpret_cast<unsigned char*>(scratch.data()),
            op.size);
        if (read < 0 || static_cast<std::size_t>(read) != op.size) {
            throw std::runtime_error("deflate_index_extract returned unexpected byte count");
        }
        total_bytes += op.size;
    }
    const auto end = std::chrono::steady_clock::now();
    return RunMetrics{
        total_bytes,
        std::chrono::duration<double>(end - begin).count(),
    };
}

RunMetrics run_seekable_zstd_reads(ZSTD_seekable* seekable, const std::vector<AccessOp>& ops,
                                   std::vector<char>& scratch) {
    const auto begin = std::chrono::steady_clock::now();
    std::size_t total_bytes = 0;
    for (const auto& op : ops) {
        const size_t read =
            ZSTD_seekable_decompress(seekable, scratch.data(), op.size, static_cast<unsigned long long>(op.pos));
        if (ZSTD_isError(read) || read != op.size) {
            throw std::runtime_error(std::string("ZSTD_seekable_decompress failed: ") +
                                     (ZSTD_isError(read) ? ZSTD_getErrorName(read)
                                                         : "unexpected byte count"));
        }
        total_bytes += op.size;
    }
    const auto end = std::chrono::steady_clock::now();
    return RunMetrics{
        total_bytes,
        std::chrono::duration<double>(end - begin).count(),
    };
}

void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0
        << " [output_csv] [repeats] [n_operations] [file_size]\n"
        << "Defaults: output_csv=locality_access_comparison.csv repeats=7 "
           "n_operations=2048 file_size=1048576\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--help") {
            print_usage(argv[0]);
            return 0;
        }

        const std::string out_csv = argc > 1 ? argv[1] : "locality_access_comparison.csv";
        const std::size_t repeats = argc > 2 ? static_cast<std::size_t>(std::stoull(argv[2])) : 7;
        const std::size_t n_operations =
            argc > 3 ? static_cast<std::size_t>(std::stoull(argv[3])) : (1u << 11);
        const std::size_t file_size =
            argc > 4 ? static_cast<std::size_t>(std::stoull(argv[4])) : (1u << 20);

        const double gamma_shape = 2.0;
        const double gamma_scale = 2048.0;
        const std::size_t region_size = 1u << 13;
        const std::vector<std::size_t> n_switch_values = {1,   2,   4,   8,   16,
                                                           32,  64,  128, 256, 512};

        const std::vector<char> payload = build_payload(file_size);
        std::vector<char> scratch(region_size);

        const std::string compio_path = make_temp_path(".compio");
        const std::string gzip_path = make_temp_path(".gz");
        const std::string zstd_path = make_temp_path(".seek.zst");
        const std::string compio_wal = compio_path + ".wal";

        write_compio_archive(compio_path, payload);
        write_gzip_file(gzip_path, payload, region_size);
        write_seekable_zstd_file(zstd_path, payload, static_cast<unsigned>(region_size));

        FILE* zran_file_for_index = std::fopen(gzip_path.c_str(), "rb");
        if (!zran_file_for_index) {
            throw std::runtime_error("fopen failed for zran index build");
        }
        struct deflate_index* zran_index = nullptr;
        const int access_points = deflate_index_build(
            zran_file_for_index, static_cast<off_t>(region_size), &zran_index);
        std::fclose(zran_file_for_index);
        if (access_points < 1 || zran_index == nullptr) {
            throw std::runtime_error("deflate_index_build failed");
        }

        std::ofstream csv(out_csv);
        if (!csv.is_open()) {
            deflate_index_free(zran_index);
            throw std::runtime_error("failed to open output CSV: " + out_csv);
        }
        csv << "backend,n_switch,repeat,n_operations,total_bytes,elapsed_sec,throughput_mib_s,"
               "avg_latency_us\n";

        std::cout << "Prepared datasets:\n"
                  << "  compio: " << compio_path << " ("
                  << std::filesystem::file_size(compio_path) << " bytes)\n"
                  << "  zran(gzip): " << gzip_path << " ("
                  << std::filesystem::file_size(gzip_path) << " bytes), access_points="
                  << access_points << "\n"
                  << "  seekable_zstd: " << zstd_path << " ("
                  << std::filesystem::file_size(zstd_path) << " bytes)\n"
                  << "Running locality sweep...\n";

        for (std::size_t n_switch : n_switch_values) {
            for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
                const auto ops = build_ops(1000 + repeat, n_operations, file_size, gamma_shape,
                                           gamma_scale, region_size, n_switch);

                const std::size_t rotate = repeat % 3;
                const std::vector<int> order = {
                    static_cast<int>((0 + rotate) % 3),
                    static_cast<int>((1 + rotate) % 3),
                    static_cast<int>((2 + rotate) % 3),
                };

                for (int backend_id : order) {
                    RunMetrics metrics;
                    const char* backend = "";
                    if (backend_id == 0) {
                        backend = "compio";
                        metrics = run_compio_reads(compio_path, ops, scratch);
                    } else if (backend_id == 1) {
                        backend = "zran";
                        FILE* zran_file = std::fopen(gzip_path.c_str(), "rb");
                        if (!zran_file) {
                            throw std::runtime_error("fopen failed for zran read run");
                        }
                        metrics = run_zran_reads(zran_file, zran_index, ops, scratch);
                        std::fclose(zran_file);
                    } else {
                        backend = "seekable_zstd";
                        FILE* zstd_file = std::fopen(zstd_path.c_str(), "rb");
                        if (!zstd_file) {
                            throw std::runtime_error("fopen failed for seekable zstd read run");
                        }
                        ZSTD_seekable* seekable = ZSTD_seekable_create();
                        if (!seekable) {
                            std::fclose(zstd_file);
                            throw std::runtime_error("ZSTD_seekable_create failed");
                        }
                        const size_t init = ZSTD_seekable_initFile(seekable, zstd_file);
                        if (ZSTD_isError(init)) {
                            ZSTD_seekable_free(seekable);
                            std::fclose(zstd_file);
                            throw std::runtime_error(std::string("ZSTD_seekable_initFile failed: ") +
                                                     ZSTD_getErrorName(init));
                        }
                        metrics = run_seekable_zstd_reads(seekable, ops, scratch);
                        ZSTD_seekable_free(seekable);
                        std::fclose(zstd_file);
                    }

                    const double mib = static_cast<double>(metrics.total_bytes) / (1024.0 * 1024.0);
                    const double throughput_mib_s = mib / metrics.elapsed_sec;
                    const double avg_latency_us =
                        metrics.elapsed_sec * 1e6 / static_cast<double>(n_operations);

                    csv << backend << "," << n_switch << "," << repeat << "," << n_operations << ","
                        << metrics.total_bytes << "," << metrics.elapsed_sec << ","
                        << throughput_mib_s << "," << avg_latency_us << "\n";
                }
            }
            std::cout << "  n_switch=" << n_switch << " done\n";
        }

        csv.close();
        deflate_index_free(zran_index);

        remove_if_exists(compio_path);
        remove_if_exists(compio_wal);
        remove_if_exists(gzip_path);
        remove_if_exists(zstd_path);

        std::cout << "Wrote CSV: " << out_csv << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
