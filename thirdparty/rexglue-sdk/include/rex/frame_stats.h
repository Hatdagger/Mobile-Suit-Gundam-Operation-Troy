/**
 * @file        rex/frame_stats.h
 *
 * @brief       Guest frame rate measurement (counts guest swaps), for the FPS
 *              readout and the debug overlay. Header-only so the graphics
 *              and UI libraries share one instance without link changes.
 */
#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>

namespace rex::frame_stats {

struct Snapshot {
  double fps = 0;
  double average_ms = 0;
  // Longest single frame in the last measuring window (shows stutter).
  double worst_ms = 0;
  uint64_t frame_count = 0;
};

namespace detail {
struct State {
  std::mutex mutex;
  std::chrono::steady_clock::time_point last_frame;
  std::chrono::steady_clock::time_point window_start;
  uint32_t window_frames = 0;
  double window_worst_ms = 0;
  uint64_t frame_count = 0;
  Snapshot published;
};
inline State& GetState() {
  static State state;
  return state;
}
}  // namespace detail

// Called once per guest frame (VdSwap), from the GPU thread.
inline void NoteFrame() {
  constexpr double kWindowSeconds = 0.5;
  auto now = std::chrono::steady_clock::now();
  detail::State& state = detail::GetState();
  std::lock_guard lock(state.mutex);
  ++state.frame_count;
  if (state.frame_count == 1) {
    state.last_frame = state.window_start = now;
    return;
  }
  double frame_ms = std::chrono::duration<double, std::milli>(now - state.last_frame).count();
  state.last_frame = now;
  ++state.window_frames;
  if (frame_ms > state.window_worst_ms) {
    state.window_worst_ms = frame_ms;
  }
  double window_s = std::chrono::duration<double>(now - state.window_start).count();
  if (window_s >= kWindowSeconds) {
    state.published.fps = state.window_frames / window_s;
    state.published.average_ms = window_s * 1000.0 / state.window_frames;
    state.published.worst_ms = state.window_worst_ms;
    state.window_start = now;
    state.window_frames = 0;
    state.window_worst_ms = 0;
  }
  state.published.frame_count = state.frame_count;
}

inline Snapshot Get() {
  detail::State& state = detail::GetState();
  std::lock_guard lock(state.mutex);
  return state.published;
}

}  // namespace rex::frame_stats
