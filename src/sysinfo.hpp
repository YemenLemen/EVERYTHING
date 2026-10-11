// sysinfo.hpp - read-only system stats + systemd unit helpers for deckctl.
// No UI code in here, so it can be tested without notcurses.
#pragma once

#include <cstdint>
#include <string>

struct SysStats {
  std::string hostname;
  double uptime_s = 0;
  double load1 = 0, load5 = 0, load15 = 0;
  double cpu_pct = 0;  // average since the previous sample()
  uint64_t mem_total_kb = 0, mem_avail_kb = 0;
  uint64_t disk_total = 0, disk_free = 0;  // bytes, filesystem of "/"
  double temp_c = -1;                      // < 0 means "not available"
};

class SysMonitor {
 public:
  SysStats sample();  // call about once per second

 private:
  uint64_t prev_idle_ = 0, prev_total_ = 0;
  bool have_prev_ = false;
};

enum class UnitState { Active, Inactive, Failed, Unknown };

struct ServiceInfo {
  std::string unit;
  UnitState state = UnitState::Unknown;
};

UnitState query_unit(const std::string& unit);  // `systemctl is-active`
// Restarts a unit (uses `sudo -n` when not root). On failure, msg gets a short reason.
bool restart_unit(const std::string& unit, std::string& msg);

std::string format_duration(double seconds);  // "3d 04h 12m"
std::string format_bytes(uint64_t bytes);     // "1.2G"
const char* state_name(UnitState s);
