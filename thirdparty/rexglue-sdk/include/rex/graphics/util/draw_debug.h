/**
 * Draw debugging helpers (Operation Troy recomp).
 *
 * - gpu_draw_log: logs every distinct (vertex shader, pixel shader) pair once,
 *   and warns when its vertex data holds values D3D would read as NaN/infinity.
 * - gpu_draw_skip_shaders: comma separated shader hashes; draws using any of
 *   them (as vertex or pixel shader) are not drawn.
 * - gpu_draw_hunt: press F9 (or create logs/hunt_start) while a scene is
 *   on screen. Skips each shader pair in turn and saves a small screenshot of
 *   each step to logs/hunt/, so the culprit can be found from the pictures.
 */

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>

namespace rex::ui {
struct RawImage;
}

namespace rex::graphics {

class DrawDebug {
 public:
  using TranslatePhysicalFn = std::function<const uint8_t*(uint32_t physical_address)>;

  // Called for every draw. Returns true if the draw must be skipped.
  bool OnDraw(const RegisterFile& regs, const Shader& vertex_shader, const Shader* pixel_shader,
              xenos::PrimitiveType primitive_type, uint32_t index_count,
              const TranslatePhysicalFn& translate);

  // Called once per presented frame.
  void OnFrameEnd(const std::function<bool(ui::RawImage&)>& capture);

  // Exposed for unit tests: decodes a Xenos float16 (no inf/NaN).
  static float XenosHalfToFloat(uint16_t h);

 private:
  struct PairKey {
    uint64_t vs;
    uint64_t ps;
    bool operator==(const PairKey& o) const { return vs == o.vs && ps == o.ps; }
  };
  struct PairKeyHash {
    size_t operator()(const PairKey& k) const {
      return size_t(k.vs * 0x9E3779B97F4A7C15ull ^ (k.ps + 0x632BE59BD9B4E019ull));
    }
  };
  struct PairInfo {
    uint64_t draws = 0;
    uint64_t bad_draws = 0;
    uint32_t order = 0;
    // Distinct depth/stencil/cull states seen for this pair (logged once each, max 8).
    std::vector<std::array<uint32_t, 8>> states;
  };

  void ParseSkipList();
  bool AnyDebugEnabled() const;
  // Returns a description of suspicious vertex values, empty if none.
  std::string CheckVertices(const RegisterFile& regs, const Shader& vertex_shader,
                            const TranslatePhysicalFn& translate);
  bool HuntStartRequested();
  void DumpVertices(const RegisterFile& regs, const Shader& vertex_shader,
                    const TranslatePhysicalFn& translate);
  uint32_t dump_count_ = 0;
  void SaveImage(const ui::RawImage& image, const std::string& name);

  std::string skip_list_source_;
  bool skip_list_parsed_ = false;
  std::unordered_set<uint64_t> skip_hashes_;

  std::unordered_map<PairKey, PairInfo, PairKeyHash> pairs_;

  // Hunt state.
  enum class HuntState { kIdle, kCollect, kStep, kDone };
  HuntState hunt_state_ = HuntState::kIdle;
  uint32_t hunt_frame_ = 0;
  uint64_t frame_number_ = 0;
  std::unordered_set<PairKey, PairKeyHash> hunt_seen_set_;
  std::vector<PairKey> hunt_pairs_;
  size_t hunt_index_ = 0;
  bool hunt_skip_active_ = false;
  PairKey hunt_skip_{0, 0};
};

}  // namespace rex::graphics
