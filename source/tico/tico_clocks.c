#include <switch.h>

#include "prefs.h"
#include "tico/tico_clocks.h"
#include "tico/tico_entry.h"

#define CPU_HZ 1785000000u
#define GPU_HZ 768000000u

static bool service_open;
static bool uses_clkrst;
static bool boost_cpu, boost_gpu;
static bool restore_cpu, restore_gpu;
static u32 original_cpu_hz, original_gpu_hz;
static Service clock_manager;
static bool clock_manager_open;

static bool open_clock_service(void) {
  if (service_open) return true;
  uses_clkrst = hosversionAtLeast(8, 0, 0);
  if (R_FAILED(uses_clkrst ? clkrstInitialize() : pcvInitialize())) {
    tico_log("clocks: %s unavailable\n", uses_clkrst ? "clkrst" : "pcv");
    return false;
  }
  service_open = true;
  return true;
}

static void close_clock_service(void) {
  if (!service_open) return;
  if (uses_clkrst)
    clkrstExit();
  else
    pcvExit();
  service_open = false;
}

static bool get_rate(bool cpu, u32 *hz) {
  if (!open_clock_service()) return false;
  if (!uses_clkrst) return R_SUCCEEDED(pcvGetClockRate(cpu ? PcvModule_CpuBus : PcvModule_GPU, hz));
  ClkrstSession session;
  if (R_FAILED(clkrstOpenSession(&session, cpu ? PcvModuleId_CpuBus : PcvModuleId_GPU, 3)))
    return false;
  const Result rc = clkrstGetClockRate(&session, hz);
  clkrstCloseSession(&session);
  return R_SUCCEEDED(rc);
}

static bool set_rate(bool cpu, u32 hz) {
  if (!open_clock_service()) return false;
  if (!uses_clkrst) return R_SUCCEEDED(pcvSetClockRate(cpu ? PcvModule_CpuBus : PcvModule_GPU, hz));
  ClkrstSession session;
  if (R_FAILED(clkrstOpenSession(&session, cpu ? PcvModuleId_CpuBus : PcvModuleId_GPU, 3)))
    return false;
  const Result rc = clkrstSetClockRate(&session, hz);
  clkrstCloseSession(&session);
  return R_SUCCEEDED(rc);
}

/* A clock manager puts its own clocks back when it sees others; sys-clk and
 * its forks take an override (command 8: module, Hz), kept until set to 0. */
static bool set_override(u32 module, u32 hz) {
  const struct {
    u32 module;
    u32 hz;
  } args = {module, hz};
  return R_SUCCEEDED(serviceDispatchIn(&clock_manager, 8, args));
}

/* Atmosphère's sm says whether a service is registered (GetService would wait) */
static bool has_service(const char *name) {
  const SmServiceName service = smEncodeName(name);
  u8 has = 0;
  const Result rc =
      hosversionAtLeast(12, 0, 0)
          ? tipcDispatchInOut(smGetServiceSessionTipc(), 65100, service, has)
          : serviceDispatchInOut(smGetServiceSession(), 65100, service, has);
  return R_SUCCEEDED(rc) && has;
}

static bool override_with_clock_manager(void) {
  static const char *const names[] = {"hoc:clk", "sysclkOC", "sysclk"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
    if (!has_service(names[i]) || R_FAILED(smGetService(&clock_manager, names[i])))
      continue;
    clock_manager_open = true;
    if ((!boost_cpu || set_override(0, CPU_HZ)) && (!boost_gpu || set_override(1, GPU_HZ))) {
      tico_log("clocks: boost through %s\n", names[i]);
      return true;
    }
    tico_log("clocks: %s refused the override\n", names[i]);
    serviceClose(&clock_manager);
    clock_manager_open = false;
  }
  return false;
}

bool tico_clocks_start(void) {
  if (!prefs_get_bool("Wrapper/BoostMode", false)) return false;
  /* raise a clock, never lower one: a faster one (an overclock) stays */
  u32 cpu_now = 0, gpu_now = 0;
  boost_cpu = !get_rate(true, &cpu_now) || cpu_now <= CPU_HZ;
  boost_gpu = !get_rate(false, &gpu_now) || gpu_now <= GPU_HZ;
  tico_log("clocks: boost mode, now cpu %u MHz gpu %u MHz\n", cpu_now / 1000000,
           gpu_now / 1000000);
  if (!boost_cpu && !boost_gpu) return false;
  if (override_with_clock_manager()) return true;
  restore_cpu = boost_cpu && get_rate(true, &original_cpu_hz);
  restore_gpu = boost_gpu && get_rate(false, &original_gpu_hz);
  const bool cpu_set = boost_cpu && set_rate(true, CPU_HZ);
  const bool gpu_set = boost_gpu && set_rate(false, GPU_HZ);
  tico_log("clocks: set cpu %d gpu %d\n", cpu_set, gpu_set);
  return cpu_set || gpu_set;
}

void tico_clocks_keep(void) {
  if (!boost_cpu && !boost_gpu) return;
  const u64 now = armGetSystemTick();
  const u64 second = armNsToTicks(1000000000ULL);
  static u64 last_pass, last_check;
  static u8 last_mode = 0xFF;
  /* a long gap: the console slept or the game was suspended */
  const bool resumed = last_pass && now - last_pass > second;
  last_pass = now;
  const u8 mode = appletGetOperationMode();
  const bool mode_changed = last_mode != 0xFF && mode != last_mode;
  last_mode = mode;
  if (clock_manager_open) {
    /* a clock manager applies clocks only when something it watches changes */
    if (resumed || mode_changed) {
      set_override(0, 0);
      set_override(1, 0);
      if (boost_cpu) set_override(0, CPU_HZ);
      if (boost_gpu) set_override(1, GPU_HZ);
    }
    return;
  }
  if (!service_open || (!resumed && !mode_changed && now - last_check < second)) return;
  last_check = now;
  u32 cpu_hz = 0, gpu_hz = 0;
  if (boost_cpu && get_rate(true, &cpu_hz) && cpu_hz < CPU_HZ) set_rate(true, CPU_HZ);
  if (boost_gpu && get_rate(false, &gpu_hz) && gpu_hz < GPU_HZ) set_rate(false, GPU_HZ);
}

void tico_clocks_restore(void) {
  if (clock_manager_open) {
    set_override(0, 0);
    set_override(1, 0);
    serviceClose(&clock_manager);
    clock_manager_open = false;
  }
  if (restore_gpu) set_rate(false, original_gpu_hz);
  if (restore_cpu) set_rate(true, original_cpu_hz);
  restore_cpu = restore_gpu = false;
  boost_cpu = boost_gpu = false;
  close_clock_service();
}
