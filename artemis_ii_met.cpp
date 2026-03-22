#include <chrono>
#include <csignal>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

volatile std::sig_atomic_t g_running = 1;
constexpr char kDefaultLaunchUtc[] = "2026-04-01T22:24:00Z";
// Alternate launch attempts kept here for later reference if needed:
// 2026-04-02T23:22:00Z
// 2026-04-04T00:00:00Z
// 2026-04-05T00:53:00Z
// 2026-04-06T01:40:00Z
// 2026-04-07T02:36:00Z
// 2026-04-30T22:06:00Z

struct MissionEvent {
  const char* name;
  std::time_t offset_seconds;
};

struct Options {
  std::string launch_timestamp = kDefaultLaunchUtc;
  bool json = false;
  bool once = false;
  bool compact = false;
};

struct TimelineState {
  const MissionEvent* previous = nullptr;
  const MissionEvent* next = nullptr;
};

struct HoldState {
  bool active = false;
  std::time_t total_seconds = 0;
  std::time_t started_at = 0;
};

constexpr const char* kColorReset = "\x1b[0m";
constexpr const char* kColorTitle = "\x1b[1;36m";
constexpr const char* kColorLabel = "\x1b[1;37m";
constexpr const char* kColorCountdown = "\x1b[1;33m";
constexpr const char* kColorElapsed = "\x1b[1;32m";
constexpr const char* kColorPhase = "\x1b[1;35m";
constexpr const char* kColorAccent = "\x1b[36m";
constexpr const char* kColorHold = "\x1b[1;31m";

class TerminalInputMode {
 public:
  TerminalInputMode() = default;

  ~TerminalInputMode() {
    disable();
  }

  bool enable() {
    if (!isatty(STDIN_FILENO)) {
      return false;
    }

    if (tcgetattr(STDIN_FILENO, &original_) != 0) {
      return false;
    }

    termios raw = original_;
    raw.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      return false;
    }

    enabled_ = true;
    return true;
  }

  void disable() {
    if (!enabled_) {
      return;
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &original_);
    enabled_ = false;
  }

  bool is_enabled() const {
    return enabled_;
  }

 private:
  termios original_{};
  bool enabled_ = false;
};

const std::vector<MissionEvent>& mission_timeline() {
  static const std::vector<MissionEvent> kTimeline = {
      {"Launch", 0},
      {"Core Stage Cutoff", 8 * 60},
      {"Trans-Lunar Injection", 2 * 60 * 60 + 10 * 60},
      {"Outbound Coast", 12 * 60 * 60},
      {"Lunar Flyby", 4 * 24 * 60 * 60 + 6 * 60 * 60},
      {"Return Coast", 5 * 24 * 60 * 60},
      {"Earth Re-entry", 10 * 24 * 60 * 60 + 5 * 60 * 60},
      {"Splashdown", 10 * 24 * 60 * 60 + 5 * 60 * 60 + 20 * 60},
  };
  return kTimeline;
}

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

std::string format_utc_time(std::time_t value) {
  std::tm utc_tm{};
#if defined(_WIN32)
  gmtime_s(&utc_tm, &value);
#else
  gmtime_r(&value, &utc_tm);
#endif

  std::ostringstream output;
  output << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

std::string format_local_time(std::time_t value) {
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &value);
#else
  localtime_r(&value, &local_tm);
#endif

  std::ostringstream output;
  output << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S %Z");
  return output.str();
}

std::string escape_json(const std::string& text) {
  std::ostringstream output;
  for (const char ch : text) {
    switch (ch) {
      case '\\':
        output << "\\\\";
        break;
      case '"':
        output << "\\\"";
        break;
      case '\n':
        output << "\\n";
        break;
      default:
        output << ch;
        break;
    }
  }
  return output.str();
}

TimelineState get_timeline_state(std::time_t elapsed_seconds) {
  TimelineState state;
  for (const auto& event : mission_timeline()) {
    if (event.offset_seconds <= elapsed_seconds) {
      state.previous = &event;
    } else {
      state.next = &event;
      break;
    }
  }
  return state;
}

Options parse_options(int argc, char* argv[]) {
  Options options;

  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--json") {
      options.json = true;
    } else if (arg == "--once") {
      options.once = true;
    } else if (arg == "--compact") {
      options.compact = true;
    } else if (arg == "--launch") {
      ++index;
      if (index >= argc) {
        throw std::invalid_argument("--launch requires a UTC timestamp");
      }
      options.launch_timestamp = argv[index];
    } else if (arg == "--help" || arg == "-h") {
      throw std::invalid_argument("");
    } else if (!arg.empty() && arg[0] == '-') {
      throw std::invalid_argument("unknown option: " + arg);
    } else if (options.launch_timestamp == kDefaultLaunchUtc) {
      options.launch_timestamp = arg;
    } else {
      throw std::invalid_argument("only one launch timestamp may be provided");
    }
  }

  return options;
}

void print_usage(const char* program_name) {
  std::cerr
      << "Usage: " << program_name << " [--json] [--once] [--compact] [--launch <launch-utc>] [launch-utc]\n"
      << "Default launch UTC: " << kDefaultLaunchUtc << '\n'
      << "Examples:\n"
      << "  " << program_name << '\n'
      << "  " << program_name << " --once\n"
      << "  " << program_name << " --json --once\n"
      << "  " << program_name << " 2026-04-01T22:24:00Z\n";
}

std::string colorize(const char* color, const std::string& text) {
  return std::string(color) + text + kColorReset;
}

std::string make_row(const std::string& label, const std::string& value) {
  std::ostringstream output;
  output << colorize(kColorLabel, label);
  if (label.size() < 14) {
    output << std::string(14 - label.size(), ' ');
  }
  output << value;
  return output.str();
}

std::time_t effective_now_seconds(std::time_t now_seconds, const HoldState& hold_state) {
  const auto active_hold = hold_state.active ? now_seconds - hold_state.started_at : 0;
  return now_seconds - hold_state.total_seconds - active_hold;
}

bool poll_spacebar(HoldState& hold_state, std::time_t now_seconds) {
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);

  timeval timeout{};
  const int ready = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
  if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &readfds)) {
    return false;
  }

  char buffer[32];
  const ssize_t bytes_read = read(STDIN_FILENO, buffer, sizeof(buffer));
  if (bytes_read <= 0) {
    return false;
  }

  bool toggled = false;
  for (ssize_t index = 0; index < bytes_read; ++index) {
    if (buffer[index] != ' ') {
      continue;
    }

    toggled = true;
    if (hold_state.active) {
      hold_state.total_seconds += now_seconds - hold_state.started_at;
      hold_state.active = false;
      hold_state.started_at = 0;
    } else {
      hold_state.active = true;
      hold_state.started_at = now_seconds;
    }
  }

  return toggled;
}

std::string build_text_output(const Options& options,
                              const std::string& launch_timestamp,
                              std::time_t launch_time,
                              std::time_t now_seconds,
                              const HoldState& hold_state) {
  const auto mission_now = effective_now_seconds(now_seconds, hold_state);
  const auto delta = static_cast<std::time_t>(std::difftime(mission_now, launch_time));
  const auto met_prefix = delta >= 0 ? "T+ " : "T- ";
  const auto met_value = format_duration(delta >= 0 ? delta : -delta);

  std::ostringstream output;

  if (options.compact) {
    if (hold_state.active) {
      output << colorize(kColorTitle, "Artemis II MET") << " "
             << colorize(kColorHold, std::string("HOLD ") + met_prefix + met_value);
    } else {
      const std::string met_color = delta >= 0 ? kColorElapsed : kColorCountdown;
      output << colorize(kColorTitle, "Artemis II MET") << " "
             << colorize(met_color.c_str(), std::string(met_prefix) + met_value);
    }
    return output.str();
  }

  const std::string met_color = delta >= 0 ? kColorElapsed : kColorCountdown;

  output << colorize(kColorTitle, "ARTEMIS II MISSION ELAPSED TIME") << '\n';
  output << colorize(kColorAccent, "================================") << '\n';
  output << make_row("MET", colorize(met_color.c_str(), std::string(met_prefix) + met_value))
         << '\n'
         << '\n';
  output << make_row("Launch UTC", format_utc_time(launch_time)) << '\n';
  output << make_row("Now UTC", format_utc_time(now_seconds)) << '\n';
  output << make_row("Mission UTC", format_utc_time(mission_now)) << '\n';
  output << make_row("Now Local", format_local_time(now_seconds)) << '\n'
         << '\n';

  if (hold_state.active) {
    output << make_row("Hold", colorize(kColorHold, "ACTIVE")) << '\n';
    output << make_row("Hold Time",
                       colorize(kColorHold, format_duration(now_seconds - hold_state.started_at)))
           << '\n';
    output << make_row("Status", colorize(kColorHold, "COUNTDOWN PAUSED - PRESS SPACE TO RESUME"))
           << '\n'
           << '\n';
  } else if (hold_state.total_seconds > 0) {
    output << make_row("Hold", colorize(kColorAccent, "Released")) << '\n';
    output << make_row("Hold Time", colorize(kColorAccent, format_duration(hold_state.total_seconds)))
           << '\n';
    output << make_row("Control", colorize(kColorAccent, "Press SPACE to enter hold")) << '\n'
           << '\n';
  } else {
    output << make_row("Control", colorize(kColorAccent, "Press SPACE to enter hold")) << '\n'
           << '\n';
  }

  if (delta < 0) {
    const std::string phase = hold_state.active ? colorize(kColorHold, "Planned Hold")
                                                : colorize(kColorPhase, "Countdown to Launch");
    output << make_row("Phase", phase) << '\n';
    output << make_row("Next Event",
                       "Launch in " +
                           colorize(hold_state.active ? kColorHold : kColorCountdown,
                                    format_duration(-delta)))
           << '\n';
    output << make_row("Attempt", launch_timestamp);
    return output.str();
  }

  const TimelineState timeline = get_timeline_state(delta);
  std::string phase = "Countdown";
  if (timeline.previous != nullptr) {
    phase = timeline.previous->name;
  }
  output << make_row("Phase",
                     hold_state.active ? colorize(kColorHold, "Planned Hold")
                                       : colorize(kColorPhase, phase))
         << '\n';

  if (timeline.next != nullptr) {
    const auto remaining = timeline.next->offset_seconds - delta;
    output << make_row("Next Event",
                       std::string(timeline.next->name) + " in " +
                           colorize(hold_state.active ? kColorHold : kColorAccent,
                                    format_duration(remaining)));
  } else {
    output << make_row("Next Event",
                       colorize(hold_state.active ? kColorHold : kColorAccent,
                                "Mission timeline complete"));
  }

  return output.str();
}

std::string build_json_output(std::time_t launch_time,
                              std::time_t now_seconds,
                              const HoldState& hold_state) {
  const auto mission_now = effective_now_seconds(now_seconds, hold_state);
  const auto delta = static_cast<std::time_t>(std::difftime(mission_now, launch_time));
  const auto met_sign = delta >= 0 ? "T+" : "T-";
  const auto met_value = format_duration(delta >= 0 ? delta : -delta);

  std::ostringstream output;
  output << "{";
  output << "\"launchUtc\":\"" << format_utc_time(launch_time) << "\",";
  output << "\"nowUtc\":\"" << format_utc_time(now_seconds) << "\",";
  output << "\"missionUtc\":\"" << format_utc_time(mission_now) << "\",";
  output << "\"nowLocal\":\"" << escape_json(format_local_time(now_seconds)) << "\",";
  output << "\"metSign\":\"" << met_sign << "\",";
  output << "\"met\":\"" << met_value << "\",";
  output << "\"deltaSeconds\":" << delta << ",";
  output << "\"hold\":{\"active\":" << (hold_state.active ? "true" : "false")
         << ",\"totalSeconds\":" << hold_state.total_seconds;
  if (hold_state.active) {
    output << ",\"currentSeconds\":" << (now_seconds - hold_state.started_at);
  }
  output << "},";

  if (delta < 0) {
    output << "\"phase\":\"" << (hold_state.active ? "Planned Hold" : "Countdown to Launch")
           << "\",";
    output << "\"nextEvent\":{\"name\":\"Launch\",\"offsetSeconds\":0,\"secondsFromNow\":" << -delta
           << "}";
  } else {
    const TimelineState timeline = get_timeline_state(delta);
    output << "\"phase\":\"";
    if (hold_state.active) {
      output << "Planned Hold";
    } else {
      output << escape_json(timeline.previous != nullptr ? timeline.previous->name : "Countdown");
    }
    output << "\",";
    if (timeline.previous != nullptr) {
      output << "\"previousEvent\":{\"name\":\"" << escape_json(timeline.previous->name)
             << "\",\"offsetSeconds\":" << timeline.previous->offset_seconds << "},";
    } else {
      output << "\"previousEvent\":null,";
    }

    if (timeline.next != nullptr) {
      output << "\"nextEvent\":{\"name\":\"" << escape_json(timeline.next->name)
             << "\",\"offsetSeconds\":" << timeline.next->offset_seconds
             << ",\"secondsFromNow\":" << (timeline.next->offset_seconds - delta) << "}";
    } else {
      output << "\"nextEvent\":null";
    }
  }

  output << "}";
  return output.str();
}

void print_frame(const Options& options,
                 std::time_t launch_time,
                 std::time_t now_seconds,
                 const HoldState& hold_state) {
  if (options.json) {
    std::cout << build_json_output(launch_time, now_seconds, hold_state) << std::endl;
    return;
  }

  const std::string text = build_text_output(options, options.launch_timestamp, launch_time,
                                             now_seconds, hold_state);
  if (options.once) {
    std::cout << text << std::endl;
    return;
  }

  if (options.compact) {
    std::cout << '\r' << std::left << std::setw(80) << text << std::flush;
  } else {
    std::cout << "\x1b[2J\x1b[H" << text << "\n\r" << std::flush;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  Options options;
  try {
    options = parse_options(argc, argv);
  } catch (const std::invalid_argument& error) {
    if (std::string(error.what()).empty()) {
      print_usage(argv[0]);
      return 0;
    }

    std::cerr << "Error: " << error.what() << '\n';
    print_usage(argv[0]);
    return 1;
  }

  std::time_t launch_time = 0;
  try {
    launch_time = parse_utc_timestamp(options.launch_timestamp);
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    print_usage(argv[0]);
    return 1;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  HoldState hold_state;
  TerminalInputMode terminal_input;
  if (!options.once && !options.json) {
    terminal_input.enable();
  }

  do {
    const auto now = std::chrono::system_clock::now();
    const auto now_seconds = std::chrono::system_clock::to_time_t(now);
    if (terminal_input.is_enabled()) {
      poll_spacebar(hold_state, now_seconds);
    }
    print_frame(options, launch_time, now_seconds, hold_state);

    if (options.once || options.json) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
  } while (g_running);

  if (!options.once && !options.json && options.compact) {
    std::cout << '\n';
  }

  return 0;
}
