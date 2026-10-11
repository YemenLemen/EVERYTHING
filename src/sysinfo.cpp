#include "sysinfo.hpp"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {

// Units come from the command line, so only allow plain unit-name characters.
bool valid_unit(const std::string& u) {
  if (u.empty() || u.size() > 128 || u[0] == '-') return false;
  for (unsigned char c : u) {
    if (!(std::isalnum(c) || c == '@' || c == '.' || c == '_' || c == ':' || c == '-'))
      return false;
  }
  return true;
}

// Runs argv directly (no shell). Returns exit code, or -1 on failure.
// stdout+stderr end up in `out`.
int run_capture(const std::vector<std::string>& args, std::string& out) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) dup2(devnull, STDIN_FILENO);
    close(fds[0]);
    close(fds[1]);
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(fds[1]);
  char buf[256];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string first_line_trimmed(const std::string& s) {
  size_t end = s.find('\n');
  std::string line = s.substr(0, end);
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
  size_t start = 0;
  while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) ++start;
  return line.substr(start);
}

}  // namespace

SysStats SysMonitor::sample() {
  SysStats s;

  char host[256] = {0};
  if (gethostname(host, sizeof host - 1) == 0) s.hostname = host;

  {
    std::ifstream f("/proc/uptime");
    f >> s.uptime_s;
  }
  {
    std::ifstream f("/proc/loadavg");
    f >> s.load1 >> s.load5 >> s.load15;
  }
  {
    // First line: cpu user nice system idle iowait irq softirq steal ...
    std::ifstream f("/proc/stat");
    std::string tag;
    f >> tag;
    uint64_t v[8] = {0};
    for (auto& x : v) f >> x;
    uint64_t idle = v[3] + v[4];
    uint64_t total = 0;
    for (auto x : v) total += x;
    if (have_prev_ && total > prev_total_) {
      double dt = static_cast<double>(total - prev_total_);
      double di = static_cast<double>(idle - prev_idle_);
      s.cpu_pct = 100.0 * (1.0 - di / dt);
      if (s.cpu_pct < 0) s.cpu_pct = 0;
      if (s.cpu_pct > 100) s.cpu_pct = 100;
    }
    prev_idle_ = idle;
    prev_total_ = total;
    have_prev_ = true;
  }
  {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
      unsigned long long kb = 0;
      if (std::sscanf(line.c_str(), "MemTotal: %llu kB", &kb) == 1) s.mem_total_kb = kb;
      else if (std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kb) == 1) s.mem_avail_kb = kb;
    }
  }
  {
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
      s.disk_total = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
      s.disk_free = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    }
  }
  {
    std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
    long milli = 0;
    if (f >> milli) s.temp_c = static_cast<double>(milli) / 1000.0;
  }
  return s;
}

UnitState query_unit(const std::string& unit) {
  if (!valid_unit(unit)) return UnitState::Unknown;
  std::string out;
  run_capture({"systemctl", "is-active", unit}, out);
  std::string word = first_line_trimmed(out);
  if (word == "active") return UnitState::Active;
  if (word == "inactive") return UnitState::Inactive;
  if (word == "failed") return UnitState::Failed;
  return UnitState::Unknown;
}

bool restart_unit(const std::string& unit, std::string& msg) {
  if (!valid_unit(unit)) {
    msg = "bad unit name";
    return false;
  }
  std::vector<std::string> args;
  if (geteuid() != 0) {
    args.push_back("sudo");
    args.push_back("-n");  // never prompt; fail fast instead
  }
  args.push_back("systemctl");
  args.push_back("restart");
  args.push_back(unit);
  std::string out;
  int rc = run_capture(args, out);
  if (rc == 0) {
    msg.clear();
    return true;
  }
  msg = first_line_trimmed(out);
  if (msg.empty()) msg = "exit code " + std::to_string(rc);
  return false;
}

std::string format_duration(double seconds) {
  long total = static_cast<long>(seconds);
  long d = total / 86400, h = (total % 86400) / 3600, m = (total % 3600) / 60;
  char buf[48];
  if (d > 0) std::snprintf(buf, sizeof buf, "%ldd %02ldh %02ldm", d, h, m);
  else if (h > 0) std::snprintf(buf, sizeof buf, "%ldh %02ldm", h, m);
  else std::snprintf(buf, sizeof buf, "%ldm", m);
  return buf;
}

std::string format_bytes(uint64_t bytes) {
  static const char* units[] = {"B", "K", "M", "G", "T"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, u == 0 ? "%.0f%s" : "%.1f%s", v, units[u]);
  return buf;
}

const char* state_name(UnitState s) {
  switch (s) {
    case UnitState::Active: return "active";
    case UnitState::Inactive: return "inactive";
    case UnitState::Failed: return "failed";
    default: return "unknown";
  }
}
