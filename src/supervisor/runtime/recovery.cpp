#include "recovery.h"

#include <algorithm>
#include <limits>

recovery_decision_t evaluate_recovery(
    const recovery_config_t &config, recovery_state_t *state,
    unsigned long exit_code, unsigned long long now_ms,
    unsigned long long run_duration_ms, unsigned long random_value) {
  recovery_decision_t decision;
  if (! state || config.mode == recovery_mode_t::never) return decision;
  if (config.mode == recovery_mode_t::on_failure && ! exit_code) {
    decision.result = recovery_result_t::clean_exit;
    return decision;
  }

  if (config.reset_after_ms && run_duration_ms >= config.reset_after_ms) {
    state->consecutive_restarts = 0;
    state->restart_times_ms.clear();
  }

  const unsigned long long window_start =
      now_ms > config.window_ms ? now_ms - config.window_ms : 0;
  std::erase_if(state->restart_times_ms,
                [window_start](unsigned long long timestamp) {
                  return timestamp < window_start;
                });
  if (config.maximum_restarts &&
      state->restart_times_ms.size() >= config.maximum_restarts) {
    decision.result = recovery_result_t::rate_limited;
    return decision;
  }

  unsigned long long delay = config.initial_delay_ms;
  for (unsigned long i = 0; i < state->consecutive_restarts; ++i) {
    if (! config.multiplier ||
        delay > config.maximum_delay_ms / config.multiplier) {
      delay = config.maximum_delay_ms;
      break;
    }
    delay *= config.multiplier;
  }
  delay = std::min<unsigned long long>(delay, config.maximum_delay_ms);
  if (delay && config.jitter_percent) {
    unsigned long long range = delay * config.jitter_percent / 100;
    unsigned long long span = range * 2 + 1;
    long long offset = static_cast<long long>(random_value % span) -
                       static_cast<long long>(range);
    long long jittered = static_cast<long long>(delay) + offset;
    delay = jittered > 0 ? static_cast<unsigned long long>(jittered) : 0;
  }
  delay = std::min<unsigned long long>(delay, config.maximum_delay_ms);
  decision.result = recovery_result_t::restart;
  decision.delay_ms = static_cast<unsigned long>(std::min<unsigned long long>(
      delay, std::numeric_limits<unsigned long>::max()));
  ++state->consecutive_restarts;
  state->restart_times_ms.push_back(now_ms);
  return decision;
}
