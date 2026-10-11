// deckctl - proof-of-concept TUI for the "central brain" Linux box.
// Build: make     Run: ./deckctl [unit ...]    (default units: mosquitto sshd)
//
// Keys: left/right = tab, up/down = select, Enter = act, q = quit.
// Uses the notcurses C API directly (the C++ ncpp wrapper is a thin layer over
// the same calls, so porting later is mechanical).

#include <notcurses/notcurses.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "sysinfo.hpp"

namespace {

struct Rgb {
  uint8_t r, g, b;
};
struct Theme {
  const char* name;
  Rgb fg, dim, warn;
};

const Theme kThemes[] = {
    {"GREEN", {0x2b, 0xff, 0x6a}, {0x15, 0x85, 0x38}, {0xff, 0xd8, 0x4a}},
    {"AMBER", {0xff, 0xb0, 0x2e}, {0x85, 0x5b, 0x18}, {0xff, 0x5a, 0x3c}},
    {"MONO", {0xe0, 0xe0, 0xe0}, {0x70, 0x70, 0x70}, {0xff, 0x70, 0x70}},
};
const int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));

enum TabId { kStatus = 0, kServices = 1, kSettings = 2, kTabCount = 3 };
const char* const kTabNames[kTabCount] = {"STATUS", "SERVICES", "SETTINGS"};

struct App {
  notcurses* nc = nullptr;
  ncplane* plane = nullptr;
  int tab = kStatus;
  int theme = 0;
  int sel[kTabCount] = {0, 0, 0};  // selected row per tab
  int confirm = -1;                // services row waiting for confirmation
  std::string msg;                 // footer message
  SysStats stats;
  std::vector<ServiceInfo> services;
  bool running = true;
};

// ---- small drawing helpers -------------------------------------------------

void set_fg(ncplane* p, Rgb c) { ncplane_set_fg_rgb8(p, c.r, c.g, c.b); }

void put(ncplane* p, int y, int x, const std::string& s) {
  if (x < 0) x = 0;
  ncplane_putstr_yx(p, y, x, s.c_str());
}

std::string repeat(const char* glyph, int n) {
  std::string s;
  for (int i = 0; i < n; ++i) s += glyph;
  return s;
}

void draw_gauge(ncplane* p, const Theme& t, int y, int x, int width, double frac) {
  frac = std::clamp(frac, 0.0, 1.0);
  int filled = static_cast<int>(frac * width + 0.5);
  set_fg(p, frac > 0.85 ? t.warn : t.fg);
  put(p, y, x, repeat("█", filled));
  set_fg(p, t.dim);
  put(p, y, x + filled, repeat("░", width - filled));
}

// ---- views -----------------------------------------------------------------

void draw_status(App& a, const Theme& t, int top, int cols) {
  ncplane* p = a.plane;
  const SysStats& s = a.stats;
  const int vx = 12;                             // value column
  const int bw = std::clamp(cols - 44, 10, 30);  // gauge width
  const int tx = vx + bw + 2;                    // text after gauge
  char buf[96];
  int y = top;

  auto label = [&](const char* l) {
    set_fg(p, t.dim);
    put(p, y, 2, l);
    set_fg(p, t.fg);
  };

  label("HOST");
  put(p, y++, vx, s.hostname);

  label("UPTIME");
  put(p, y++, vx, format_duration(s.uptime_s));

  label("LOAD");
  std::snprintf(buf, sizeof buf, "%.2f  %.2f  %.2f", s.load1, s.load5, s.load15);
  put(p, y++, vx, buf);

  y++;
  label("CPU");
  draw_gauge(p, t, y, vx, bw, s.cpu_pct / 100.0);
  set_fg(p, t.fg);
  std::snprintf(buf, sizeof buf, "%3.0f%%", s.cpu_pct);
  put(p, y++, tx, buf);

  label("MEM");
  uint64_t used_kb = s.mem_total_kb > s.mem_avail_kb ? s.mem_total_kb - s.mem_avail_kb : 0;
  double mem_frac = s.mem_total_kb ? static_cast<double>(used_kb) / s.mem_total_kb : 0;
  draw_gauge(p, t, y, vx, bw, mem_frac);
  set_fg(p, t.fg);
  put(p, y++, tx, format_bytes(used_kb * 1024) + " / " + format_bytes(s.mem_total_kb * 1024));

  label("DISK /");
  uint64_t used = s.disk_total > s.disk_free ? s.disk_total - s.disk_free : 0;
  double disk_frac = s.disk_total ? static_cast<double>(used) / s.disk_total : 0;
  draw_gauge(p, t, y, vx, bw, disk_frac);
  set_fg(p, t.fg);
  put(p, y++, tx, format_bytes(s.disk_free) + " free");

  y++;
  label("TEMP");
  if (s.temp_c < 0) {
    put(p, y, vx, "n/a");
  } else {
    set_fg(p, s.temp_c > 80 ? t.warn : t.fg);
    std::snprintf(buf, sizeof buf, "%.1f C", s.temp_c);
    put(p, y, vx, buf);
  }
}

void draw_services(App& a, const Theme& t, int top) {
  ncplane* p = a.plane;
  if (a.services.empty()) {
    set_fg(p, t.dim);
    put(p, top, 2, "no units configured (pass them as arguments)");
    return;
  }
  for (size_t i = 0; i < a.services.size(); ++i) {
    const ServiceInfo& sv = a.services[i];
    int y = top + static_cast<int>(i);
    bool selected = static_cast<int>(i) == a.sel[kServices];
    Rgb state_color = sv.state == UnitState::Active   ? t.fg
                      : sv.state == UnitState::Failed ? t.warn
                                                      : t.dim;
    set_fg(p, selected ? t.fg : t.dim);
    put(p, y, 2, selected ? ">" : " ");
    set_fg(p, state_color);
    put(p, y, 4, "●");
    set_fg(p, selected ? t.fg : t.dim);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%-28s %s", sv.unit.c_str(), state_name(sv.state));
    put(p, y, 6, buf);
  }
  set_fg(p, t.dim);
  put(p, top + static_cast<int>(a.services.size()) + 1, 2, "Enter = restart (asks first)");
}

void draw_settings(App& a, const Theme& t, int top) {
  ncplane* p = a.plane;
  std::string rows[2] = {std::string("Theme: ") + t.name, "Quit"};
  for (int i = 0; i < 2; ++i) {
    bool selected = i == a.sel[kSettings];
    set_fg(p, selected ? t.fg : t.dim);
    put(p, top + i, 2, selected ? ">" : " ");
    put(p, top + i, 4, rows[i]);
  }
}

void draw(App& a) {
  ncplane* p = a.plane;
  unsigned urows = 0, ucols = 0;
  ncplane_dim_yx(p, &urows, &ucols);
  const int rows = static_cast<int>(urows), cols = static_cast<int>(ucols);
  const Theme& t = kThemes[a.theme];

  ncplane_erase(p);
  ncplane_set_bg_default(p);
  ncplane_set_styles(p, NCSTYLE_NONE);

  if (rows < 12 || cols < 50) {
    set_fg(p, t.warn);
    put(p, 0, 0, "terminal too small (need 50x12)");
    return;
  }

  // header: title left, hostname right
  set_fg(p, t.fg);
  ncplane_set_styles(p, NCSTYLE_BOLD);
  put(p, 0, 1, " DECKCTL // CENTRAL BRAIN ");
  ncplane_set_styles(p, NCSTYLE_NONE);
  set_fg(p, t.dim);
  put(p, 0, cols - static_cast<int>(a.stats.hostname.size()) - 2, a.stats.hostname);

  // tab row
  int x = 2;
  for (int i = 0; i < kTabCount; ++i) {
    std::string label = i == a.tab ? std::string("[ ") + kTabNames[i] + " ]"
                                   : std::string("  ") + kTabNames[i] + "  ";
    set_fg(p, i == a.tab ? t.fg : t.dim);
    if (i == a.tab) ncplane_set_styles(p, NCSTYLE_BOLD);
    put(p, 1, x, label);
    ncplane_set_styles(p, NCSTYLE_NONE);
    x += static_cast<int>(label.size()) + 1;
  }
  set_fg(p, t.dim);
  put(p, 2, 0, repeat("─", cols));

  const int top = 4;
  switch (a.tab) {
    case kStatus: draw_status(a, t, top, cols); break;
    case kServices: draw_services(a, t, top); break;
    case kSettings: draw_settings(a, t, top); break;
  }

  // footer: message if any, otherwise key hints
  set_fg(p, t.dim);
  put(p, rows - 2, 0, repeat("─", cols));
  if (!a.msg.empty()) {
    set_fg(p, t.warn);
    put(p, rows - 1, 1, a.msg);
  } else {
    put(p, rows - 1, 1, "←/→ tab   ↑/↓ select   Enter act   q quit");
  }
}

// ---- input -----------------------------------------------------------------

void refresh_services(App& a) {
  for (auto& sv : a.services) sv.state = query_unit(sv.unit);
}

int row_count(const App& a) {
  switch (a.tab) {
    case kServices: return static_cast<int>(a.services.size());
    case kSettings: return 2;
    default: return 0;
  }
}

void activate(App& a) {
  if (a.tab == kServices) {
    if (a.services.empty()) return;
    int i = a.sel[kServices];
    const std::string& unit = a.services[i].unit;
    if (a.confirm != i) {  // first press: ask
      a.confirm = i;
      a.msg = "restart " + unit + "?  Enter = yes, any other key = cancel";
      return;
    }
    a.confirm = -1;  // second press: do it
    std::string why;
    a.msg = restart_unit(unit, why) ? "restarted " + unit : "restart failed: " + why;
    refresh_services(a);
  } else if (a.tab == kSettings) {
    if (a.sel[kSettings] == 0) a.theme = (a.theme + 1) % kThemeCount;
    else a.running = false;
  }
}

void handle_key(App& a, uint32_t key) {
  bool enter = key == NCKEY_ENTER || key == '\n' || key == '\r';
  if (!enter && a.confirm >= 0) {  // anything but Enter cancels a pending confirm
    a.confirm = -1;
    a.msg.clear();
  }
  if (key == 'q') {
    a.running = false;
  } else if (key == NCKEY_RIGHT || key == NCKEY_TAB) {
    a.tab = (a.tab + 1) % kTabCount;
    a.msg.clear();
  } else if (key == NCKEY_LEFT) {
    a.tab = (a.tab + kTabCount - 1) % kTabCount;
    a.msg.clear();
  } else if (key == NCKEY_UP || key == NCKEY_DOWN) {
    int n = row_count(a);
    if (n > 0) a.sel[a.tab] = (a.sel[a.tab] + (key == NCKEY_DOWN ? 1 : n - 1)) % n;
  } else if (enter) {
    activate(a);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setlocale(LC_ALL, "");

  App app;
  for (int i = 1; i < argc; ++i) app.services.push_back({argv[i], UnitState::Unknown});
  if (app.services.empty()) {
    app.services.push_back({"mosquitto.service", UnitState::Unknown});
    app.services.push_back({"sshd.service", UnitState::Unknown});
  }

  notcurses_options opts{};
  opts.flags = NCOPTION_SUPPRESS_BANNERS;
  app.nc = notcurses_init(&opts, nullptr);
  if (!app.nc) return 1;
  app.plane = notcurses_stdplane(app.nc);

  using clock = std::chrono::steady_clock;
  SysMonitor mon;
  auto last = clock::now() - std::chrono::seconds(2);  // sample on first loop
  int svc_ticks = 3;                                   // refresh services on first loop

  while (app.running) {
    auto now = clock::now();
    if (now - last >= std::chrono::seconds(1)) {
      app.stats = mon.sample();
      if (++svc_ticks >= 3) {  // systemctl calls are cheap but not free
        refresh_services(app);
        svc_ticks = 0;
      }
      last = now;
    }

    draw(app);
    notcurses_render(app.nc);

    ncinput ni{};
    timespec ts{0, 250 * 1000 * 1000};  // wake up at least 4x per second
    uint32_t key = notcurses_get(app.nc, &ts, &ni);
    if (key == static_cast<uint32_t>(-1)) break;  // input error
    if (key == 0) continue;                       // timeout: just redraw
    if (ni.evtype == NCTYPE_RELEASE) continue;    // ignore key-release events
    handle_key(app, key);
  }

  notcurses_stop(app.nc);
  return 0;
}
