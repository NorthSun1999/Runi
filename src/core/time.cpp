#include "runi/core/time.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace runi {
namespace {

std::tm local_time(std::time_t value) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}

std::tm utc_time(std::time_t value) {
    std::tm result{};
#ifdef _WIN32
    gmtime_s(&result, &value);
#else
    gmtime_r(&value, &result);
#endif
    return result;
}

}  // namespace

std::string now_utc() {
    const auto current = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(current);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        current.time_since_epoch()).count() % 1000000;
    const auto tm = utc_time(seconds);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setw(6) << std::setfill('0') << micros << "+00:00";
    return output.str();
}

std::string local_timestamp() {
    const auto seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const auto tm = local_time(seconds);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return output.str();
}

std::string random_hex(std::size_t length) {
    static thread_local std::mt19937_64 generator(std::random_device{}());
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        result.push_back(digits[generator() & 0x0fU]);
    }
    return result;
}

std::string new_session_id() { return local_timestamp() + "-" + random_hex(6); }
std::string new_task_id() { return "task_" + local_timestamp() + "-" + random_hex(6); }
std::string new_run_id() { return "run_" + local_timestamp() + "-" + random_hex(6); }
std::string new_checkpoint_id() { return "ckpt_" + random_hex(8); }

}  // namespace runi
