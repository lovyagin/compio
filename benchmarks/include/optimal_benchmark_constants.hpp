#ifndef OPTIMAL_BENCHMARK_CONSTANTS_HPP_
#define OPTIMAL_BENCHMARK_CONSTANTS_HPP_

static constexpr std::size_t N_OPERATIONS = 1 << 13;
static constexpr std::size_t FILE_SIZE = 1 << 25;
static constexpr std::size_t N_SWITCH = 1;
static constexpr double GAMMA_SHAPE = 2.0;
static constexpr double GAMMA_SCALE = 1 << 12;
static constexpr std::size_t REGION_SIZE = 1 << 17;

static constexpr bool IS_WRITE = false;
static constexpr bool DISABLE_CACHE = true;

static constexpr int MIN_ITERATIONS = 30;
static constexpr double MAX_SECONDS = 60.0;
static constexpr int MAX_ITERATIONS = 1000;

static constexpr const char *SAMPLE_FILE = BENCHMARK_DATA_DIR "/enwik9";

inline std::pair<std::vector<char>, std::size_t> load_random_sample_data(std::minstd_rand &rng) {
    std::ifstream sf(SAMPLE_FILE, std::ios::binary | std::ios::ate);
    if (!sf.is_open())
        throw std::runtime_error("could not open sample file");
    std::streamsize sample_file_size = sf.tellg();
    sf.close();

    std::size_t max_offset = static_cast<std::size_t>(sample_file_size) - FILE_SIZE;
    std::uniform_int_distribution<std::size_t> offset_dist(0, max_offset);
    std::size_t offset = offset_dist(rng);

    std::ifstream file(SAMPLE_FILE, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<char> data(FILE_SIZE);
    if (!file.read(data.data(), FILE_SIZE))
        throw std::runtime_error("failed to read sample data");
    return {std::move(data), offset};
}

#endif // OPTIMAL_BENCHMARK_CONSTANTS
