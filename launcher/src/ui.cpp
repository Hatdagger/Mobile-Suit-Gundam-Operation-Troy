#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "util.h"

namespace ui {

namespace {

ImFont* g_fonts[5] = {};
ImVec2 g_display{kCanvasW, kCanvasH};
ImVec2 g_origin{0, 0};
float g_scale = 1.f;
float g_time = 0.f;
Glyphs g_glyphs = Glyphs::Keyboard;

ImDrawList* DL() { return ImGui::GetBackgroundDrawList(); }
ImFont* F(Font f) { return g_fonts[static_cast<int>(f)]; }
float Px(float size) { return std::max(1.f, std::round(size * g_scale)); }

// Length in bytes of the UTF-8 sequence starting with c.
int Utf8Len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

}  // namespace

ImU32 WithAlpha(ImU32 c, float alpha) {
  const float a = float((c >> 24) & 0xFF) * std::clamp(alpha, 0.f, 1.f);
  return (c & 0x00FFFFFF) | (ImU32(a + 0.5f) << 24);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  t = std::clamp(t, 0.f, 1.f);
  auto ch = [&](int shift) {
    const float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
    return ImU32(x + (y - x) * t + 0.5f) << shift;
  };
  return ch(0) | ch(8) | ch(16) | ch(24);
}

void LoadFonts() {
  ImGuiIO& io = ImGui::GetIO();
  auto add = [&](const char* name, float size) -> ImFont* {
    size_t n = 0;
    const unsigned char* data = FindEmbeddedFile(name, &n);
    if (!data) return io.Fonts->AddFontDefault();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    std::snprintf(cfg.Name, sizeof cfg.Name, "%s", name);
    return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(n), size, &cfg);
  };
  g_fonts[int(Font::Body)] = add("FiraSans-Regular.ttf", 18.f);  // first font = ImGui's default
  g_fonts[int(Font::Title)] = add("Anton-Regular.ttf", 44.f);
  g_fonts[int(Font::Heading)] = add("FiraSans-SemiBold.ttf", 24.f);
  g_fonts[int(Font::Label)] = add("FiraSans-Medium.ttf", 14.f);
  g_fonts[int(Font::Mono)] = add("AnonymousPro-Regular.ttf", 15.f);
}

void BeginFrame(ImVec2 display_size, float time, Glyphs glyphs) {
  g_display = display_size;
  g_scale = std::min(display_size.x / kCanvasW, display_size.y / kCanvasH);
  g_origin = ImVec2(std::floor((display_size.x - kCanvasW * g_scale) * 0.5f),
                    std::floor((display_size.y - kCanvasH * g_scale) * 0.5f));
  g_time = time;
  g_glyphs = glyphs;
}

float Time() { return g_time; }
Glyphs CurrentGlyphs() { return g_glyphs; }

ImVec2 P(float x, float y) { return ImVec2(g_origin.x + x * g_scale, g_origin.y + y * g_scale); }
float S(float v) { return v * g_scale; }
ImVec2 ToCanvas(ImVec2 s) { return ImVec2((s.x - g_origin.x) / g_scale, (s.y - g_origin.y) / g_scale); }

// --- Text -------------------------------------------------------------------------

ImVec2 Measure(Font f, float size, std::string_view text) {
  if (text.empty()) return ImVec2(0, size);
  ImVec2 s = F(f)->CalcTextSizeA(Px(size), FLT_MAX, 0.f, text.data(), text.data() + text.size());
  return ImVec2(s.x / g_scale, s.y / g_scale);
}

void Text(Font f, float size, float x, float y, ImU32 color, std::string_view text, float align) {
  if (text.empty() || (color & 0xFF000000) == 0) return;
  if (align != 0.f) x -= Measure(f, size, text).x * align;
  ImVec2 p = P(x, y);
  DL()->AddText(F(f), Px(size), ImVec2(std::round(p.x), std::round(p.y)), color, text.data(), text.data() + text.size());
}

void TextSpaced(Font f, float size, float x, float y, ImU32 color, std::string_view text, float spacing) {
  size_t i = 0;
  while (i < text.size()) {
    const int len = std::min<int>(Utf8Len(static_cast<unsigned char>(text[i])), int(text.size() - i));
    std::string_view ch = text.substr(i, size_t(len));
    Text(f, size, x, y, color, ch);
    x += Measure(f, size, ch).x + spacing;
    i += size_t(len);
  }
}

float WrappedHeight(Font f, float size, float width, std::string_view text, float line_height) {
  return TextWrapped(f, size, 0, 0, width, 0, text, line_height, 0);  // colour 0 draws nothing
}

float TextWrapped(Font f, float size, float x, float y, float width, ImU32 color, std::string_view text,
                  float line_height, int max_lines) {
  const float step = size * line_height;
  float cy = y;
  int lines = 0;
  size_t para_start = 0;
  while (para_start <= text.size()) {
    size_t para_end = text.find('\n', para_start);
    if (para_end == std::string_view::npos) para_end = text.size();
    std::string_view para = text.substr(para_start, para_end - para_start);
    std::string line;
    size_t i = 0;
    auto flush = [&](bool last_of_para) {
      if (max_lines > 0 && lines == max_lines - 1 && !last_of_para) line = Fit(f, size, line + " ...", width);
      Text(f, size, x, cy, color, line);
      cy += step;
      ++lines;
      line.clear();
    };
    while (i <= para.size()) {
      size_t sp = para.find(' ', i);
      if (sp == std::string_view::npos) sp = para.size();
      std::string word(para.substr(i, sp - i));
      // A word wider than the whole line (a long path) is broken where it fills the line.
      while (word.size() > 1 && Measure(f, size, word).x > width) {
        if (!line.empty()) {
          flush(false);
          if (max_lines > 0 && lines >= max_lines) return cy - y;
        }
        size_t cut = word.size() - 1;
        while (cut > 1 && Measure(f, size, std::string_view(word).substr(0, cut)).x > width) {
          --cut;
          while (cut > 1 && (static_cast<unsigned char>(word[cut]) & 0xC0) == 0x80) --cut;
        }
        line = word.substr(0, cut);
        word = word.substr(cut);
        flush(false);
        if (max_lines > 0 && lines >= max_lines) return cy - y;
      }
      std::string candidate = line.empty() ? word : line + " " + word;
      if (!line.empty() && Measure(f, size, candidate).x > width) {
        flush(false);
        if (max_lines > 0 && lines >= max_lines) return cy - y;
        line = word;
      } else {
        line = candidate;
      }
      i = sp + 1;
    }
    flush(true);
    if (max_lines > 0 && lines >= max_lines) return cy - y;
    para_start = para_end + 1;
  }
  return cy - y;
}

std::string Fit(Font f, float size, std::string_view text, float max_width) {
  if (Measure(f, size, text).x <= max_width) return std::string(text);
  std::string s(text);
  while (!s.empty()) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0x80)) s.pop_back();
    if (Measure(f, size, s + "...").x <= max_width) return s + "...";
  }
  return "...";
}

// --- Shapes -----------------------------------------------------------------------

void Rect(float x, float y, float w, float h, ImU32 color) { DL()->AddRectFilled(P(x, y), P(x + w, y + h), color); }

void RectGradientH(float x, float y, float w, float h, ImU32 left, ImU32 right) {
  DL()->AddRectFilledMultiColor(P(x, y), P(x + w, y + h), left, right, right, left);
}

void RectGradientV(float x, float y, float w, float h, ImU32 top, ImU32 bottom) {
  DL()->AddRectFilledMultiColor(P(x, y), P(x + w, y + h), top, top, bottom, bottom);
}

void Line(float x1, float y1, float x2, float y2, ImU32 color, float thickness) {
  DL()->AddLine(P(x1, y1), P(x2, y2), color, std::max(1.f, S(thickness)));
}

void Chamfer(float x, float y, float w, float h, float cut, ImU32 fill, ImU32 edge, float thickness) {
  ImVec2 pts[6] = {P(x + cut, y), P(x + w, y), P(x + w, y + h - cut), P(x + w - cut, y + h), P(x, y + h), P(x, y + cut)};
  if (fill & 0xFF000000) DL()->AddConvexPolyFilled(pts, 6, fill);
  if (edge & 0xFF000000) DL()->AddPolyline(pts, 6, edge, ImDrawFlags_Closed, std::max(1.f, S(thickness)));
}

void Brackets(float x, float y, float w, float h, float len, ImU32 color, float t) {
  Line(x, y, x + len, y, color, t);
  Line(x, y, x, y + len, color, t);
  Line(x + w, y, x + w - len, y, color, t);
  Line(x + w, y, x + w, y + len, color, t);
  Line(x, y + h, x + len, y + h, color, t);
  Line(x, y + h, x, y + h - len, color, t);
  Line(x + w, y + h, x + w - len, y + h, color, t);
  Line(x + w, y + h, x + w, y + h - len, color, t);
}

void Circle(float cx, float cy, float r, ImU32 color, bool filled, float thickness) {
  if (filled)
    DL()->AddCircleFilled(P(cx, cy), S(r), color, 0);
  else
    DL()->AddCircle(P(cx, cy), S(r), color, 0, std::max(1.f, S(thickness)));
}

void Triangle(float cx, float cy, float size, int direction, ImU32 color) {
  const float h = size * 0.5f;
  ImVec2 a, b, c;
  switch (direction) {
    case 0: a = P(cx - h * 0.8f, cy - h); b = P(cx + h * 0.8f, cy); c = P(cx - h * 0.8f, cy + h); break;
    case 1: a = P(cx - h, cy - h * 0.8f); b = P(cx + h, cy - h * 0.8f); c = P(cx, cy + h * 0.8f); break;
    case 2: a = P(cx + h * 0.8f, cy - h); b = P(cx + h * 0.8f, cy + h); c = P(cx - h * 0.8f, cy); break;
    default: a = P(cx, cy - h * 0.8f); b = P(cx + h, cy + h * 0.8f); c = P(cx - h, cy + h * 0.8f); break;
  }
  DL()->AddTriangleFilled(a, b, c, color);
}

void PushClip(float x, float y, float w, float h) { DL()->PushClipRect(P(x, y), P(x + w, y + h), true); }
void PopClip() { DL()->PopClipRect(); }

// --- Building blocks --------------------------------------------------------------

void Background() {
  ImDrawList* dl = DL();
  dl->AddRectFilledMultiColor(ImVec2(0, 0), g_display, col::Rgba(12, 22, 38), col::Rgba(12, 22, 38),
                              col::Rgba(3, 6, 11), col::Rgba(3, 6, 11));
  // Grid across the whole window, aligned to the canvas.
  const float step = S(40.f);
  if (step >= 4.f) {
    float x0 = std::fmod(g_origin.x, step), y0 = std::fmod(g_origin.y, step);
    int i = 0;
    for (float x = x0; x < g_display.x; x += step, ++i)
      dl->AddLine(ImVec2(x, 0), ImVec2(x, g_display.y), col::Rgba(80, 170, 255, (i % 4 == 0) ? 16 : 8));
    i = 0;
    for (float y = y0; y < g_display.y; y += step, ++i)
      dl->AddLine(ImVec2(0, y), ImVec2(g_display.x, y), col::Rgba(80, 170, 255, (i % 4 == 0) ? 16 : 8));
  }
  // Diagonal hatching, bottom right.
  PushClip(900, 560, 380, 160);
  for (int k = 0; k < 40; ++k) {
    const float x = 900.f + float(k) * 14.f;
    Line(x, 720, x + 160, 560, col::Rgba(54, 195, 255, 9), 4.f);
  }
  PopClip();
  // Slow, faint scan band.
  const float t = std::fmod(g_time, 9.f) / 9.f;
  const float band_y = -120.f + t * (kCanvasH + 240.f);
  dl->AddRectFilledMultiColor(ImVec2(0, P(0, band_y).y), ImVec2(g_display.x, P(0, band_y + 60).y), col::Rgba(54, 195, 255, 0),
                              col::Rgba(54, 195, 255, 0), col::Rgba(54, 195, 255, 7), col::Rgba(54, 195, 255, 7));
  dl->AddRectFilledMultiColor(ImVec2(0, P(0, band_y + 60).y), ImVec2(g_display.x, P(0, band_y + 120).y), col::Rgba(54, 195, 255, 7),
                              col::Rgba(54, 195, 255, 7), col::Rgba(54, 195, 255, 0), col::Rgba(54, 195, 255, 0));
  // Vignette.
  const ImU32 dark = col::Rgba(0, 0, 0, 105), clear = col::Rgba(0, 0, 0, 0);
  const float v = S(140.f);
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(v, g_display.y), dark, clear, clear, dark);
  dl->AddRectFilledMultiColor(ImVec2(g_display.x - v, 0), g_display, clear, dark, dark, clear);
  dl->AddRectFilledMultiColor(ImVec2(0, g_display.y - v), g_display, clear, clear, dark, dark);
}

void Header(std::string_view section, std::string_view status, bool online) {
  RectGradientV(0, 0, kCanvasW, 110, col::Rgba(3, 8, 15, 210), col::Rgba(3, 8, 15, 0));
  TextSpaced(Font::Label, 13, 66, 24, col::kAccent, "MOBILE SUIT GUNDAM", 3.2f);
  Text(Font::Title, 46, 64, 38, col::kText, "OPERATION TROY");
  const float tw = Measure(Font::Title, 46, "OPERATION TROY").x;
  Line(64 + tw + 24, 50, 64 + tw + 24, 88, col::kAccentDim, 2.f);
  TextSpaced(Font::Heading, 20, 64 + tw + 42, 57, col::kAccent, section, 2.4f);

  const float right = 1216.f;
  const float chip_w = Measure(Font::Label, 13, online ? "ONLINE" : "OFFLINE").x + 34.f;
  Chip(right, 64, online ? "ONLINE" : "OFFLINE", online ? col::kGood : col::kTextDim);
  Text(Font::Label, 13, right - chip_w - 16, 56, col::kTextFaint, status, 1.f);

  Line(64, 102, right, 102, col::kPanelEdge, 1.f);
  Rect(64, 100, tw, 3, col::kAccent);
  for (int i = 0; i < 24; ++i) Line(right - float(i) * 8.f, 97, right - float(i) * 8.f, 102, col::kPanelEdge, 1.f);
}

void Footer(std::string_view hint, const std::vector<Prompt>& prompts) {
  Line(64, 662, 1216, 662, col::kPanelEdge, 1.f);
  const float pw = PromptsWidth(prompts, 24);
  if (!hint.empty()) Text(Font::Body, 15, 64, 680, col::kTextFaint, Fit(Font::Body, 15, hint, 1216 - 64 - pw - 40));
  DrawPrompts(prompts, 1216, 690, 24);
}

float ButtonGlyph(Button b, float x, float cy, float size) {
  const float r = size * 0.5f;
  auto keycap = [&](std::string_view label) {
    const float w = std::max(size, Measure(Font::Label, size * 0.5f, label).x + size * 0.6f);
    Chamfer(x, cy - r, w, size, size * 0.18f, col::Rgba(20, 36, 56), col::kTextDim, 1.f);
    Text(Font::Label, size * 0.5f, x + w * 0.5f, cy - size * 0.33f, col::kText, label, 0.5f);
    return w;
  };
  auto face = [&](ImU32 color, std::string_view letter) {
    Circle(x + r, cy, r, col::Rgba(14, 24, 36));
    Circle(x + r, cy, r - 1.f, color, false, 2.f);
    Text(Font::Heading, size * 0.62f, x + r, cy - size * 0.40f, color, letter, 0.5f);
    return size;
  };
  auto ps_face = [&](ImU32 color, int shape) {
    Circle(x + r, cy, r, col::Rgba(14, 24, 36));
    Circle(x + r, cy, r - 1.f, color, false, 2.f);
    const float k = r * 0.42f;
    const float cx = x + r;
    switch (shape) {
      case 0:  // cross
        Line(cx - k, cy - k, cx + k, cy + k, color, 2.f);
        Line(cx - k, cy + k, cx + k, cy - k, color, 2.f);
        break;
      case 1: Circle(cx, cy, k, color, false, 2.f); break;
      case 2:  // square
        Line(cx - k, cy - k, cx + k, cy - k, color, 2.f);
        Line(cx + k, cy - k, cx + k, cy + k, color, 2.f);
        Line(cx + k, cy + k, cx - k, cy + k, color, 2.f);
        Line(cx - k, cy + k, cx - k, cy - k, color, 2.f);
        break;
      default:  // triangle
        Line(cx, cy - k, cx + k, cy + k * 0.8f, color, 2.f);
        Line(cx + k, cy + k * 0.8f, cx - k, cy + k * 0.8f, color, 2.f);
        Line(cx - k, cy + k * 0.8f, cx, cy - k, color, 2.f);
        break;
    }
    return size;
  };
  if (b == Button::Move) {
    // D-pad
    const float a = size * 0.18f, l = size * 0.5f, cx = x + r;
    Rect(cx - a, cy - l, a * 2, l * 2, col::kTextDim);
    Rect(cx - l, cy - a, l * 2, a * 2, col::kTextDim);
    return size;
  }
  switch (g_glyphs) {
    case Glyphs::Keyboard:
      switch (b) {
        case Button::Accept: return keycap("ENTER");
        case Button::Back: return keycap("ESC");
        case Button::Alt: return keycap("Y");
        case Button::Extra: return keycap("X");
        case Button::PrevTab: return keycap("Q");
        case Button::NextTab: return keycap("E");
        case Button::Start: return keycap("F5");
        default: return 0.f;
      }
    case Glyphs::PlayStation:
      switch (b) {
        case Button::Accept: return ps_face(col::Rgba(120, 170, 255), 0);
        case Button::Back: return ps_face(col::Rgba(255, 110, 110), 1);
        case Button::Extra: return ps_face(col::Rgba(240, 140, 220), 2);
        case Button::Alt: return ps_face(col::Rgba(90, 220, 170), 3);
        case Button::PrevTab: return keycap("L1");
        case Button::NextTab: return keycap("R1");
        case Button::Start: return keycap("OPTIONS");
        default: return 0.f;
      }
    case Glyphs::Nintendo:
      switch (b) {
        case Button::Accept: return face(col::kText, "B");
        case Button::Back: return face(col::kText, "A");
        case Button::Extra: return face(col::kText, "Y");
        case Button::Alt: return face(col::kText, "X");
        case Button::PrevTab: return keycap("L");
        case Button::NextTab: return keycap("R");
        case Button::Start: return keycap("+");
        default: return 0.f;
      }
    case Glyphs::Xbox:
    default:
      switch (b) {
        case Button::Accept: return face(col::Rgba(96, 210, 96), "A");
        case Button::Back: return face(col::Rgba(255, 96, 96), "B");
        case Button::Extra: return face(col::Rgba(80, 150, 255), "X");
        case Button::Alt: return face(col::Rgba(255, 210, 70), "Y");
        case Button::PrevTab: return keycap("LB");
        case Button::NextTab: return keycap("RB");
        case Button::Start: return keycap("MENU");
        default: return 0.f;
      }
  }
}

namespace {

const char* KeyLabel(Button b) {
  if (b == Button::Move) return nullptr;
  switch (g_glyphs) {
    case Glyphs::Keyboard:
      switch (b) {
        case Button::Accept: return "ENTER";
        case Button::Back: return "ESC";
        case Button::Alt: return "Y";
        case Button::Extra: return "X";
        case Button::PrevTab: return "Q";
        case Button::NextTab: return "E";
        case Button::Start: return "F5";
        default: return nullptr;
      }
    case Glyphs::PlayStation:
      return b == Button::PrevTab ? "L1" : b == Button::NextTab ? "R1" : b == Button::Start ? "OPTIONS" : nullptr;
    case Glyphs::Nintendo:
      return b == Button::PrevTab ? "L" : b == Button::NextTab ? "R" : b == Button::Start ? "+" : nullptr;
    default:
      return b == Button::PrevTab ? "LB" : b == Button::NextTab ? "RB" : b == Button::Start ? "MENU" : nullptr;
  }
}

float GlyphWidth(Button b, float size) {
  const char* key = KeyLabel(b);
  if (!key) return size;
  return std::max(size, Measure(Font::Label, size * 0.5f, key).x + size * 0.6f);
}

float SpacedWidth(Font f, float size, std::string_view text, float spacing) {
  int n = 0;
  for (unsigned char c : text)
    if ((c & 0xC0) != 0x80) ++n;
  return Measure(f, size, text).x + spacing * float(std::max(0, n - 1));
}

constexpr float kPromptLabel = 15.f;
constexpr float kPromptSpacing = 0.6f;
constexpr float kPromptGap = 26.f;

}  // namespace

float PromptsWidth(const std::vector<Prompt>& prompts, float size) {
  float w = 0;
  for (const auto& p : prompts)
    w += GlyphWidth(p.button, size) + 8.f + SpacedWidth(Font::Label, kPromptLabel, p.label, kPromptSpacing) + kPromptGap;
  return prompts.empty() ? 0.f : w - kPromptGap;
}

void DrawPrompts(const std::vector<Prompt>& prompts, float right_x, float cy, float size) {
  float x = right_x - PromptsWidth(prompts, size);
  for (const auto& p : prompts) {
    ButtonGlyph(p.button, x, cy, size);
    x += GlyphWidth(p.button, size) + 8.f;
    TextSpaced(Font::Label, kPromptLabel, x, cy - 9.f, col::kTextDim, p.label, kPromptSpacing);
    x += SpacedWidth(Font::Label, kPromptLabel, p.label, kPromptSpacing) + kPromptGap;
  }
}

void Panel(float x, float y, float w, float h, std::string_view title) {
  Chamfer(x, y, w, h, 14.f, col::kPanel, col::kPanelEdge, 1.f);
  Brackets(x - 4, y - 4, w + 8, h + 8, 12.f, WithAlpha(col::kAccent, 0.55f), 2.f);
  if (!title.empty()) {
    const float tw = Measure(Font::Label, 13, title).x + 2.2f * float(title.size()) + 28.f;
    Chamfer(x + 14, y - 11, tw, 22, 6.f, col::Rgba(16, 52, 78), col::kAccentDim, 1.f);
    TextSpaced(Font::Label, 13, x + 28, y - 8, col::kAccent, title, 2.2f);
  }
}

void SegmentBar(float x, float y, float w, float h, float value, ImU32 color) {
  const float seg = 10.f, gap = 3.f;
  const int n = std::max(1, int((w + gap) / (seg + gap)));
  const float real_seg = (w - gap * float(n - 1)) / float(n);
  for (int i = 0; i < n; ++i) {
    const float sx = x + float(i) * (real_seg + gap);
    ImU32 c = col::Rgba(24, 42, 62);
    if (value >= 0.f) {
      const float fill = value * float(n) - float(i);
      if (fill >= 1.f)
        c = color;
      else if (fill > 0.f)
        c = Mix(col::Rgba(24, 42, 62), color, 0.25f + 0.75f * fill);
    } else {
      const float head = std::fmod(g_time * 1.4f, 1.f) * float(n + 8) - 4.f;
      const float d = std::fabs(float(i) - head);
      if (d < 4.f) c = Mix(col::Rgba(24, 42, 62), color, 1.f - d / 4.f);
    }
    Rect(sx, y, real_seg, h, c);
  }
}

void Spinner(float cx, float cy, float r, ImU32 color) {
  ImDrawList* dl = DL();
  const float a0 = g_time * 5.f;
  dl->PathArcTo(P(cx, cy), S(r), a0, a0 + 4.2f, 24);
  dl->PathStroke(color, 0, std::max(1.f, S(2.5f)));
}

void Chip(float right_x, float cy, std::string_view text, ImU32 color) {
  const float tw = Measure(Font::Label, 13, text).x;
  const float w = tw + 34.f, h = 22.f;
  const float x = right_x - w;
  Chamfer(x, cy - h * 0.5f, w, h, 6.f, WithAlpha(color, 0.12f), WithAlpha(color, 0.7f), 1.f);
  Circle(x + 12, cy, 3.5f, color);
  Text(Font::Label, 13, x + 22, cy - 8.f, color, text);
}

void MenuHighlight(float x, float y, float w, float h, float amount) {
  if (amount <= 0.01f) return;
  RectGradientH(x, y, w, h, WithAlpha(col::kAccent, 0.30f * amount), WithAlpha(col::kAccent, 0.0f));
  Rect(x, y, 4, h, WithAlpha(col::kAccent, amount));
  Line(x, y, x + w * 0.7f, y, WithAlpha(col::kAccent, 0.45f * amount), 1.f);
  Line(x, y + h, x + w * 0.5f, y + h, WithAlpha(col::kAccent, 0.25f * amount), 1.f);
}

}  // namespace ui
