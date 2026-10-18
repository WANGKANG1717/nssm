#ifndef RECOVERY_H
#define RECOVERY_H

#include "config.h"

#include <cstdint>
#include <vector>

struct recovery_state_t {
  unsigned long consecutive_restarts = 0;
  std::vector<unsigned long long> restart_times_ms;
};

enum class recovery_result_t {
  restart,
  mode_disabled,
  clean_exit,
  rate_limited,
};

struct recovery_decision_t {
  recovery_result_t result = recovery_result_t::mode_disabled;
  unsigned long delay_ms = 0;

  bool should_restart() const { return result == recovery_result_t::restart; }
};

recovery_decision_t evaluate_recovery(
    const recovery_config_t &config, recovery_state_t *state,
    unsigned long exit_code, unsigned long long now_ms,
    unsigned long long run_duration_ms, unsigned long random_value);

#endif
