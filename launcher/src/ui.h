// Drawing for the launcher: an original "mobile suit cockpit" look built from
// shapes and the embedded fonts (no game assets). Everything is laid out on a
// 1280x720 canvas that is scaled to the window.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"

namespace ui {

constexpr float kCanvasW = 1280.f;
constexpr float kCanvasH = 720.f;

enum class Font { Title, Heading, Body, Label, Mono };

namespace col {
constexpr ImU32 Rgba(int r, int g, int b, int a = 255) {
  return ImU32(r) | (ImU32(g) << 8) | (ImU32(b) << 16) | (ImU32(a) << 24);
}
constexpr ImU32 kText = Rgba(232, 241, 250);
constexpr ImU32 kTextDim = Rgba(142, 163, 184);
constexpr ImU32 kTextFaint = Rgba(86, 105, 124);
constexpr ImU32 kAccent = Rgba(54, 195, 255);
constexpr ImU32 kAccentDim = Rgba(29, 110, 153);
constexpr ImU32 kWarn = Rgba(255, 180, 60);
constexpr ImU32 kBad = Rgba(255, 86, 86);
constexpr ImU32 kGood = Rgba(76, 230, 138);
constexpr ImU32 kPanel = Rgba(9, 20, 35, 215);
constexpr ImU32 kPanelEdge = Rgba(35, 65, 94);
}  // namespace col

ImU32 WithAlpha(ImU32 c, float alpha);  // multiplies the colour's alpha
ImU32 Mix(ImU32 a, ImU32 b, float t);

enum class Glyphs { Keyboard, Xbox, PlayStation, Nintendo };
enum class Button { Accept, Back, Alt, Extra, PrevTab, NextTab, Start, Move };
struct Prompt {
  Button button;
  std::string label;
};

void LoadFonts();  // call once after ImGui::CreateContext()
void BeginFrame(ImVec2 display_size, float time, Glyphs glyphs);
float Time();
Glyphs CurrentGlyphs();

// Canvas <-> screen.
ImVec2 P(float x, float y);
float S(float v);
ImVec2 ToCanvas(ImVec2 screen);

// Text. align: 0 = left, 0.5 = centre, 1 = right edge at x.
ImVec2 Measure(Font f, float size, std::string_view text);
void Text(Font f, float size, float x, float y, ImU32 color, std::string_view text, float align = 0.f);
void TextSpaced(Font f, float size, float x, float y, ImU32 color, std::string_view text, float spacing);
float TextWrapped(Font f, float size, float x, float y, float width, ImU32 color, std::string_view text,
                  float line_height = 1.3f, int max_lines = 0);  // returns height used
float WrappedHeight(Font f, float size, float width, std::string_view text, float line_height = 1.3f);
std::string Fit(Font f, float size, std::string_view text, float max_width);  // ends with "..." if cut

// Shapes (canvas units).
void Rect(float x, float y, float w, float h, ImU32 color);
void RectGradientH(float x, float y, float w, float h, ImU32 left, ImU32 right);
void RectGradientV(float x, float y, float w, float h, ImU32 top, ImU32 bottom);
void Line(float x1, float y1, float x2, float y2, ImU32 color, float thickness = 1.f);
void Chamfer(float x, float y, float w, float h, float cut, ImU32 fill, ImU32 edge = 0, float thickness = 1.f);
void Brackets(float x, float y, float w, float h, float len, ImU32 color, float thickness = 2.f);
void Circle(float cx, float cy, float r, ImU32 color, bool filled = true, float thickness = 1.f);
void Triangle(float cx, float cy, float size, int direction, ImU32 color);  // 0 right, 1 down, 2 left, 3 up
void PushClip(float x, float y, float w, float h);
void PopClip();

// Building blocks.
void Background();
void Header(std::string_view section, std::string_view status, bool online);
void Footer(std::string_view hint, const std::vector<Prompt>& prompts);
float ButtonGlyph(Button b, float x, float cy, float size);  // returns width used
float PromptsWidth(const std::vector<Prompt>& prompts, float size);
void DrawPrompts(const std::vector<Prompt>& prompts, float right_x, float cy, float size);
void Panel(float x, float y, float w, float h, std::string_view title = {});
void SegmentBar(float x, float y, float w, float h, float value, ImU32 color);  // value < 0: busy sweep
void Spinner(float cx, float cy, float r, ImU32 color);
void Chip(float right_x, float cy, std::string_view text, ImU32 color);  // small status tag

// A row in a vertical menu: highlight, label, optional description.
struct MenuRowStyle {
  bool selected = false;
  bool enabled = true;
  float highlight = 0.f;  // 0..1 animation
};
void MenuHighlight(float x, float y, float w, float h, float amount);

}  // namespace ui
