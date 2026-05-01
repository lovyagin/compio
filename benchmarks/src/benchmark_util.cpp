#include "benchmark_util.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

std::string lower(std::string &s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string upper(std::string &s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

benchmark_context build_config_from_file(std::string fn, compio_config *config) {
    std::ifstream file(fn);

    if (!file.is_open()) {
        throw std::runtime_error("failed to load config from file");
    }

    benchmark_context bc;

    std::string line;
    while (std::getline(file, line)) {
        size_t pos = line.find('=');
        if (pos == std::string::npos) {
            throw std::runtime_error("invalid line in config file: " + line);
        }

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        lower(key);
        lower(value);

        std::pair<std::string, std::string> context_elem;
        context_elem.first = key;
        context_elem.second = value;
        upper(context_elem.first);
        upper(context_elem.second);
        bc.push_back(context_elem);

        if (key == "compression") {
            if (value == "zlib") {
                compio_build_zlib_compressor(&config->compressor);
            } else if (value == "dummy") {
                compio_build_dummy_compressor(&config->compressor);
            } else if (value == "lz4") {
                compio_build_lz4_compressor(&config->compressor);
            } else if (value == "zstd") {
                compio_build_zstd_compressor(&config->compressor);
            } else if (value == "brotli") {
                compio_build_brotli_compressor(&config->compressor);
            } else {
                goto error_key_value;
            }
        } else if (key == "compression_level") {
            config->compressor.level = std::atoi(value.c_str());
        } else if (key == "cache_size__nodes") {
            config->cache_size__nodes = std::atoi(value.c_str());
        } else if (key == "cache_size__blocks") {
            config->cache_size__blocks = std::atoi(value.c_str());
        } else if (key == "b_tree_degree") {
            config->b_tree_degree = std::atoi(value.c_str());
        } else if (key == "block_size") {
            config->block_size = std::atoi(value.c_str());
        } else if (key == "block_size__minimum") {
            config->block_size__minimum = std::atoi(value.c_str());
        } else if (key == "block_size__maximum") {
            config->block_size__maximum = std::atoi(value.c_str());
        } else if (key == "fill_holes_with_zeros") {
            if (value == "true") {
                config->fill_holes_with_zeros = true;
            } else if (value == "false") {
                config->fill_holes_with_zeros = false;
            } else {
                goto error_key_value;
            }
        } else if (key == "allocation_strategy") {
            if (value == "first_fit") {
                config->allocation_strategy = COMPIO_ALLOC_FIRST_FIT;
            } else if (value == "next_fit") {
                config->allocation_strategy = COMPIO_ALLOC_NEXT_FIT;
            } else if (value == "best_fit") {
                config->allocation_strategy = COMPIO_ALLOC_BEST_FIT;
            } else if (value == "worst_fit") {
                config->allocation_strategy = COMPIO_ALLOC_WORST_FIT;
            } else {
                goto error_key_value;
            }
        } else if (key == "fragmentation_threshold") {
            config->fragmentation_threshold = std::atoi(value.c_str());
        } else {
            goto error_key_value;
        }

        continue;
    error_key_value:
        throw std::runtime_error("invalid key-value in config file: " + key + "=" + value);
    }

    file.close();

    return bc;
}

std::pair<const char*, std::size_t> load_webster_data() {
    static std::vector<char> data;
    static bool loaded = false;
    if (loaded) {
        return {data.data(), data.size()};
    }

    const char* webster_path = BENCHMARK_DATA_DIR "/webster";
    const std::filesystem::path exePath = std::filesystem::canonical(webster_path);
    const std::filesystem::path exeDir = exePath.parent_path();
    const std::filesystem::path dataPath = exeDir / "webster";

    std::ifstream file;
    file.open(dataPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("could not open benchmark data file at: " + std::string(dataPath.c_str()));
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    data.resize(static_cast<std::size_t>(size));
    if (!file.read(data.data(), size)) {
        throw std::runtime_error("failed to read webster data file");
    }

    loaded = true;
    return {data.data(), data.size()};
}
