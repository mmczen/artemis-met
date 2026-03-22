#include <chrono>
#include <csignal>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_running = 1;
constexpr char kDefaultLaunchUtc[] = "2026-04-01T22:24:00Z";

void handle_signal(int) {
  g_running = 0;
}

std::time_t parse_utc_timestamp(const std::string& text) {
  if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
    throw std::invalid_argument(
        "timestamp must be in UTC ISO-8601 form: YYYY-MM-DDTHH:MM:SSZ");
  }

  std::tm tm{};
  std::istringstream input(text);
  input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  if (input.fail()) {
    throw std::invalid_argument("failed to parse launch timestamp");
  }

#if defined(_WIN32)
  return _mkgmtime(&tm);
#else
  return timegm(&tm);
#endif
}

std::string format_duration(std::time_t total_seconds) {
  const auto days = total_seconds / 86400;
  total_seconds %= 86400;
  const auto hours = total_seconds / 3600;
  total_seconds %= 3600;
  const auto minutes = total_seconds / 60;
  const auto seconds = total_seconds % 60;

  std::ostringstream output;
  output << std::setfill('0')
         << std::setw(3) << days << '/'
         << std::setw(2) << hours << ':'
         << std::setw(2) << minutes << ':'
         << std::setw(2) << seconds;
  return output.str();
}

void print_usage(const char* program_name) {
  std::cerr
      << "Usage: " << program_name << " [launch-utc]\n"
      << "Default: " << kDefaultLaunchUtc << '\n'
      << "Example: " << program_name << " 2026-04-01T12:00:00Z\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc > 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::time_t launch_time = 0;
  try {
    const std::string launch_timestamp = argc == 2 ? argv[1] : kDefaultLaunchUtc;
    launch_time = parse_utc_timestamp(launch_timestamp);
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    print_usage(argv[0]);
    return 1;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  while (g_running) {
    const auto now = std::chrono::system_clock::now();
    const auto now_seconds = std::chrono::system_clock::to_time_t(now);
    const auto delta = static_cast<std::time_t>(std::difftime(now_seconds, launch_time));

    std::ostringstream line;
    if (delta >= 0) {
      line << "Artemis II MET  T+ " << format_duration(delta);
    } else {
      line << "Artemis II MET  T- " << format_duration(-delta);
    }

    std::cout << '\r' << std::left << std::setw(40) << line.str() << std::flush;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::cout << '\n';
  return 0;
}
