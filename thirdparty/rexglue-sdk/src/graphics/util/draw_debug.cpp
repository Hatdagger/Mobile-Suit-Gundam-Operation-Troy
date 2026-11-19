/**
 * Draw debugging helpers (Operation Troy recomp). See draw_debug.h.
 */

#include <rex/graphics/util/draw_debug.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(gpu_draw_log, false, "GPU/Debug",
                    "Log each distinct vertex+pixel shader pair once, and warn about vertex "
                    "data D3D would read as NaN/infinity");
REXCVAR_DEFINE_STRING(gpu_draw_skip_shaders, "", "GPU/Debug",
                      "Comma separated shader hashes (16 hex digits) whose draws are skipped");
REXCVAR_DEFINE_STRING(gpu_draw_dump_vs, "", "GPU/Debug",
                      "Vertex shader hash (16 hex digits): log its float constants for the first "
                      "draws that use it");
REXCVAR_DEFINE_BOOL(gpu_draw_hunt, false, "GPU/Debug",
                    "F9 (or creating logs/hunt_start) skips each shader pair in turn and "
                    "saves a screenshot per step to logs/hunt/");
REXCVAR_DEFINE_INT32(gpu_draw_hunt_step_frames, 12, "GPU/Debug",
                     "Frames each shader pair stays hidden during a hunt (screenshot on the last)");

namespace rex::graphics {

namespace {

constexpr uint32_t kMaxCheckedVertices = 4096;
constexpr int kHuntCollectFrames = 90;

std::filesystem::path LogFolder() {
  return rex::filesystem::GetExecutableFolder() / "logs";
}

const char* VertexFormatName(xenos::VertexFormat f) {
  switch (f) {
    case xenos::VertexFormat::k_8_8_8_8:
      return "8_8_8_8";
    case xenos::VertexFormat::k_2_10_10_10:
      return "2_10_10_10";
    case xenos::VertexFormat::k_10_11_11:
      return "10_11_11";
    case xenos::VertexFormat::k_11_11_10:
      return "11_11_10";
    case xenos::VertexFormat::k_16_16:
      return "16_16";
    case xenos::VertexFormat::k_16_16_16_16:
      return "16_16_16_16";
    case xenos::VertexFormat::k_16_16_FLOAT:
      return "16_16_FLOAT";
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
      return "16_16_16_16_FLOAT";
    case xenos::VertexFormat::k_32:
      return "32";
    case xenos::VertexFormat::k_32_32:
      return "32_32";
    case xenos::VertexFormat::k_32_32_32_32:
      return "32_32_32_32";
    case xenos::VertexFormat::k_32_FLOAT:
      return "32_FLOAT";
    case xenos::VertexFormat::k_32_32_FLOAT:
      return "32_32_FLOAT";
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      return "32_32_32_32_FLOAT";
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      return "32_32_32_FLOAT";
    default:
      return "?";
  }
}

}  // namespace

float DrawDebug::XenosHalfToFloat(uint16_t h) {
  uint32_t sign = h >> 15;
  uint32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  float value;
  if (exponent == 0) {
    value = std::ldexp(float(mantissa), -24);
  } else {
    value = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
  }
  return sign ? -value : value;
}

bool DrawDebug::AnyDebugEnabled() const {
  return REXCVAR_GET(gpu_draw_log) || REXCVAR_GET(gpu_draw_hunt) ||
         !REXCVAR_GET(gpu_draw_skip_shaders).empty();
}

void DrawDebug::ParseSkipList() {
  const std::string& source = REXCVAR_GET(gpu_draw_skip_shaders);
  if (skip_list_parsed_ && source == skip_list_source_) {
    return;
  }
  skip_list_parsed_ = true;
  skip_list_source_ = source;
  skip_hashes_.clear();
  std::string token;
  auto flush = [&]() {
    // Keep hex digits only (allows "0x", spaces, quotes).
    std::string hex;
    for (size_t i = 0; i < token.size(); ++i) {
      char c = token[i];
      if (c == 'x' || c == 'X') {
        hex.clear();
        continue;
      }
      if (std::isxdigit(static_cast<unsigned char>(c))) {
        hex.push_back(c);
      }
    }
    token.clear();
    if (hex.empty() || hex.size() > 16) {
      return;
    }
    skip_hashes_.insert(std::stoull(hex, nullptr, 16));
  };
  for (char c : source) {
    if (c == ',' || c == ';') {
      flush();
    } else {
      token.push_back(c);
    }
  }
  flush();
  if (!skip_hashes_.empty()) {
    REXGPU_INFO("DrawDebug: skipping draws using {} shader hash(es)", skip_hashes_.size());
  }
}

std::string DrawDebug::CheckVertices(const RegisterFile& regs, const Shader& vertex_shader,
                                     const TranslatePhysicalFn& translate) {
  uint64_t half_exp31 = 0, float_bad = 0, half_total = 0, float_total = 0;
  float sample_value = 0.0f;
  uint32_t sample_raw = 0;
  bool have_sample = false;
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex || !binding.stride_words) {
      continue;
    }
    const uint8_t* base = translate(fetch.address << 2);
    if (!base) {
      continue;
    }
    uint32_t size_words = fetch.size;
    uint32_t vertex_count = std::min(size_words / binding.stride_words, kMaxCheckedVertices);
    auto load = [&](uint32_t word_index) -> uint32_t {
      uint32_t raw;
      std::memcpy(&raw, base + size_t(word_index) * 4, 4);
      return xenos::GpuSwap(raw, fetch.endian);
    };
    for (const Shader::VertexBinding::Attribute& attribute : binding.attributes) {
      const auto& a = attribute.fetch_instr.attributes;
      bool is_half = a.data_format == xenos::VertexFormat::k_16_16_FLOAT ||
                     a.data_format == xenos::VertexFormat::k_16_16_16_16_FLOAT;
      bool is_float = a.data_format == xenos::VertexFormat::k_32_FLOAT ||
                      a.data_format == xenos::VertexFormat::k_32_32_FLOAT ||
                      a.data_format == xenos::VertexFormat::k_32_32_32_FLOAT ||
                      a.data_format == xenos::VertexFormat::k_32_32_32_32_FLOAT;
      if (!is_half && !is_float) {
        continue;
      }
      uint32_t components = uint32_t(xenos::GetVertexFormatComponentCount(a.data_format));
      uint32_t words = is_half ? (components + 1) / 2 : components;
      uint32_t stride = a.stride ? a.stride : binding.stride_words;
      if (a.offset < 0) {
        continue;
      }
      for (uint32_t v = 0; v < vertex_count; ++v) {
        uint32_t first = v * stride + uint32_t(a.offset);
        if (first + words > size_words) {
          break;
        }
        for (uint32_t w = 0; w < words; ++w) {
          uint32_t value = load(first + w);
          if (is_half) {
            for (uint32_t h = 0; h < 2; ++h) {
              uint16_t half = uint16_t(value >> (16 * h));
              ++half_total;
              if ((half & 0x7C00) == 0x7C00) {
                ++half_exp31;
                if (!have_sample) {
                  have_sample = true;
                  sample_raw = half;
                  sample_value = XenosHalfToFloat(half);
                }
              }
            }
          } else {
            ++float_total;
            if ((value & 0x7F800000) == 0x7F800000) {
              ++float_bad;
              if (!have_sample) {
                have_sample = true;
                sample_raw = value;
                std::memcpy(&sample_value, &value, 4);
              }
            }
          }
        }
      }
    }
  }
  if (!half_exp31 && !float_bad) {
    return {};
  }
  return fmt::format(
      "float16 with exponent 31 (NaN/inf on PC, big number on Xbox): {}/{}; "
      "float32 NaN/inf: {}/{}; first raw {:08X} (Xbox value {})",
      half_exp31, half_total, float_bad, float_total, sample_raw, sample_value);
}

bool DrawDebug::OnDraw(const RegisterFile& regs, const Shader& vertex_shader,
                       const Shader* pixel_shader, xenos::PrimitiveType primitive_type,
                       uint32_t index_count, const TranslatePhysicalFn& translate) {
  if (!AnyDebugEnabled()) {
    return false;
  }
  PairKey key{vertex_shader.ucode_data_hash(), pixel_shader ? pixel_shader->ucode_data_hash() : 0};

  if (REXCVAR_GET(gpu_draw_log)) {
    auto [it, inserted] = pairs_.try_emplace(key);
    PairInfo& info = it->second;
    if (inserted) {
      info.order = uint32_t(pairs_.size());
      std::string formats;
      for (const auto& binding : vertex_shader.vertex_bindings()) {
        for (const auto& attribute : binding.attributes) {
          if (!formats.empty()) {
            formats += ' ';
          }
          formats += VertexFormatName(attribute.fetch_instr.attributes.data_format);
        }
      }
      REXGPU_INFO(
          "DrawDebug: pair #{} VS {:016X} PS {:016X} prim {} indices {} textures {} formats [{}]",
          info.order, key.vs, key.ps, uint32_t(primitive_type), index_count,
          pixel_shader ? pixel_shader->texture_bindings().size() : 0, formats);
    }
    ++info.draws;
    // Render state, raw register values: depth/stencil control, stencil front/back, cull and
    // polygon mode, clipping, viewport transform, color write mask, depth buffer format.
    std::array<uint32_t, 8> state = {
        regs[XE_GPU_REG_RB_DEPTHCONTROL],  regs[XE_GPU_REG_RB_STENCILREFMASK],
        regs[XE_GPU_REG_RB_STENCILREFMASK_BF], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
        regs[XE_GPU_REG_PA_CL_CLIP_CNTL],  regs[XE_GPU_REG_PA_CL_VTE_CNTL],
        regs[XE_GPU_REG_RB_COLOR_MASK],    regs[XE_GPU_REG_RB_DEPTH_INFO]};
    if (info.states.size() < 8 &&
        std::find(info.states.begin(), info.states.end(), state) == info.states.end()) {
      info.states.push_back(state);
      REXGPU_INFO(
          "DrawDebug: state pair #{} VS {:016X} PS {:016X} prim {} depthctl {:08X} stencil "
          "{:08X} stencil_bf {:08X} mode {:08X} clip {:08X} vte {:08X} colormask {:08X} "
          "depthinfo {:08X}",
          info.order, key.vs, key.ps, uint32_t(primitive_type), state[0], state[1], state[2],
          state[3], state[4], state[5], state[6], state[7]);
    }
    std::string bad = CheckVertices(regs, vertex_shader, translate);
    if (!bad.empty()) {
      ++info.bad_draws;
      uint64_t n = info.bad_draws;
      if (n == 1 || n == 10 || n == 100 || n == 1000 || n % 10000 == 0) {
        REXGPU_WARN("DrawDebug: BAD VERTICES pair #{} VS {:016X} PS {:016X} (bad draw {}): {}",
                    info.order, key.vs, key.ps, n, bad);
      }
    }
  }

  if (!REXCVAR_GET(gpu_draw_dump_vs).empty() && dump_count_ < 3) {
    uint64_t want = std::strtoull(REXCVAR_GET(gpu_draw_dump_vs).c_str(), nullptr, 16);
    if (want && want == key.vs) {
      ++dump_count_;
      REXGPU_INFO("DrawDebug: DUMP {} VS {:016X} PS {:016X} prim {} indices {}", dump_count_,
                  key.vs, key.ps, uint32_t(primitive_type), index_count);
      for (uint32_t i = 0; i < 256; ++i) {
        float v[4];
        bool any = false, bad = false;
        for (uint32_t j = 0; j < 4; ++j) {
          v[j] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + j);
          any |= v[j] != 0.0f;
          bad |= !std::isfinite(v[j]) || std::fabs(v[j]) > 1.0e6f;
        }
        if (any) {
          REXGPU_INFO("DrawDebug: DUMP {} c{:03} {:g} {:g} {:g} {:g}{}", dump_count_, i, v[0], v[1],
                      v[2], v[3], bad ? " <-- SUSPICIOUS" : "");
        }
      }
    }
  }

  if (REXCVAR_GET(gpu_draw_hunt)) {
    if (hunt_state_ == HuntState::kCollect && hunt_seen_set_.insert(key).second) {
      hunt_pairs_.push_back(key);
    }
    if (hunt_skip_active_ && key == hunt_skip_) {
      return true;
    }
  }

  if (!REXCVAR_GET(gpu_draw_skip_shaders).empty() || skip_list_parsed_) {
    ParseSkipList();
    if (!skip_hashes_.empty() &&
        (skip_hashes_.count(key.vs) || (key.ps && skip_hashes_.count(key.ps)))) {
      return true;
    }
  }
  return false;
}

bool DrawDebug::HuntStartRequested() {
#if defined(_WIN32)
  if (GetAsyncKeyState(VK_F9) & 0x8000) {
    return true;
  }
#endif
  if (frame_number_ % 30 == 0) {
    std::error_code ec;
    auto trigger = LogFolder() / "hunt_start";
    if (std::filesystem::exists(trigger, ec)) {
      std::filesystem::remove(trigger, ec);
      return true;
    }
  }
  return false;
}

void DrawDebug::SaveImage(const ui::RawImage& image, const std::string& name) {
  // Downscale to at most 480 pixels wide, write a 24-bit BMP.
  uint32_t factor = std::max<uint32_t>(1, (image.width + 479) / 480);
  uint32_t width = image.width / factor;
  uint32_t height = image.height / factor;
  if (!width || !height) {
    return;
  }
  uint32_t row_bytes = (width * 3 + 3) & ~uint32_t(3);
  std::vector<uint8_t> pixels(size_t(row_bytes) * height, 0);
  for (uint32_t y = 0; y < height; ++y) {
    // BMP rows go bottom-up.
    uint8_t* dest = pixels.data() + size_t(height - 1 - y) * row_bytes;
    const uint8_t* src_row = image.data.data() + size_t(y * factor) * image.stride;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* src = src_row + size_t(x * factor) * 4;
      dest[x * 3 + 0] = src[2];
      dest[x * 3 + 1] = src[1];
      dest[x * 3 + 2] = src[0];
    }
  }
  std::error_code ec;
  auto folder = LogFolder() / "hunt";
  std::filesystem::create_directories(folder, ec);
  std::ofstream out(folder / name, std::ios::binary);
  if (!out) {
    return;
  }
  auto put16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
  auto put32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  uint32_t data_size = uint32_t(pixels.size());
  out.write("BM", 2);
  put32(54 + data_size);
  put32(0);
  put32(54);
  put32(40);
  put32(width);
  put32(height);
  put16(1);
  put16(24);
  put32(0);
  put32(data_size);
  put32(2835);
  put32(2835);
  put32(0);
  put32(0);
  out.write(reinterpret_cast<const char*>(pixels.data()), data_size);
}

void DrawDebug::OnFrameEnd(const std::function<bool(ui::RawImage&)>& capture) {
  ++frame_number_;
  if (!REXCVAR_GET(gpu_draw_hunt)) {
    return;
  }
  int step_frames = std::max(4, REXCVAR_GET(gpu_draw_hunt_step_frames));
  switch (hunt_state_) {
    case HuntState::kIdle:
    case HuntState::kDone:
      if (HuntStartRequested()) {
        hunt_state_ = HuntState::kCollect;
        hunt_frame_ = 0;
        hunt_seen_set_.clear();
        hunt_pairs_.clear();
        hunt_index_ = 0;
        hunt_skip_active_ = false;
        REXGPU_INFO("DrawDebug: HUNT START - collecting shader pairs for {} frames",
                    kHuntCollectFrames);
      }
      break;
    case HuntState::kCollect:
      if (++hunt_frame_ >= uint32_t(kHuntCollectFrames)) {
        ui::RawImage image;
        if (capture(image)) {
          SaveImage(image, "hunt_000_baseline.bmp");
        }
        REXGPU_INFO("DrawDebug: HUNT found {} shader pairs; stepping {} frames each",
                    hunt_pairs_.size(), step_frames);
        for (size_t i = 0; i < hunt_pairs_.size(); ++i) {
          REXGPU_INFO("DrawDebug: HUNT step {:03} = VS {:016X} PS {:016X}", i + 1,
                      hunt_pairs_[i].vs, hunt_pairs_[i].ps);
        }
        hunt_state_ = HuntState::kStep;
        hunt_frame_ = 0;
        hunt_index_ = 0;
        if (hunt_pairs_.empty()) {
          hunt_state_ = HuntState::kDone;
        } else {
          hunt_skip_ = hunt_pairs_[0];
          hunt_skip_active_ = true;
        }
      }
      break;
    case HuntState::kStep:
      if (++hunt_frame_ >= uint32_t(step_frames)) {
        ui::RawImage image;
        if (capture(image)) {
          SaveImage(image, fmt::format("hunt_{:03}_VS{:016X}_PS{:016X}.bmp", hunt_index_ + 1,
                                       hunt_skip_.vs, hunt_skip_.ps));
        }
        hunt_frame_ = 0;
        if (++hunt_index_ >= hunt_pairs_.size()) {
          hunt_skip_active_ = false;
          hunt_state_ = HuntState::kDone;
          ui::RawImage end_image;
          if (capture(end_image)) {
            SaveImage(end_image, "hunt_999_end.bmp");
          }
          REXGPU_INFO("DrawDebug: HUNT DONE ({} steps). Press F9 to run again.",
                      hunt_pairs_.size());
        } else {
          hunt_skip_ = hunt_pairs_[hunt_index_];
        }
      }
      break;
  }
}

}  // namespace rex::graphics
