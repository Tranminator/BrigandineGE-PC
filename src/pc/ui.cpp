// Settings window (F2): tabs for Graphics, Sound and Controls, built with Dear ImGui.
// Changes apply immediately; closing the window (F2 / Esc / Close) saves settings.cfg.
#include "ui.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include "settings.h"
void platform_apply_video(void);
}

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static bool s_open;
static bool s_ready;

static const char *FONT_CANDIDATES[] = {
    "/System/Library/Fonts/Supplemental/Arial.ttf",      // macOS
    "/System/Library/Fonts/Helvetica.ttc",
    "C:\\Windows\\Fonts\\segoeui.ttf",                   // Windows
    "C:\\Windows\\Fonts\\arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   // Linux
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
};

extern "C" void ui_init(SDL_Window *win, SDL_Renderer *ren) {
    s_win = win; s_ren = ren;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;                                   // no imgui.ini next to the game
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 8; st.FrameRounding = 4; st.TabRounding = 4; st.GrabRounding = 4;
    st.WindowPadding = ImVec2(16, 14); st.ItemSpacing = ImVec2(10, 9);

    // crisp text on HiDPI: rasterise the font at pixel density, then scale back
    int ww, wh, ow, oh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    float dpi = ww ? (float)ow / ww : 1.0f;
    for (const char *path : FONT_CANDIDATES) {
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fclose(f);
        if (io.Fonts->AddFontFromFileTTF(path, 17.0f * dpi)) { io.FontGlobalScale = 1.0f / dpi; break; }
    }
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
    s_ready = true;
}

extern "C" int ui_is_open(void) { return s_open; }

static void close_ui(void) { s_open = false; settings_save(); }

extern "C" int ui_event(const SDL_Event *e) {
    if (!s_ready) return 0;
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        if (e->key.keysym.sym == SDLK_F2) { if (s_open) close_ui(); else s_open = true; return 1; }
        if (s_open && e->key.keysym.sym == SDLK_ESCAPE) { close_ui(); return 1; }
    }
    ImGui_ImplSDL2_ProcessEvent(e);
    if (!s_open) return 0;
    // while open, the UI owns keyboard, mouse and controller input
    switch (e->type) {
    case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL: case SDL_MOUSEMOTION:
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION:
        return 1;
    }
    return 0;
}

static bool slider_pct(const char *label, int *v) {
    bool ch = ImGui::SliderInt(label, v, 0, 100, "%d%%");
    if (ch) settings_apply();
    return ch;
}

static void hint(const char *t) {   /* wrapped, dimmed help text */
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
}

static void tab_graphics(void) {
    bool vsync = g_settings.vsync != 0;
    if (ImGui::Checkbox("VSync", &vsync)) { g_settings.vsync = vsync; platform_apply_video(); }
    hint("On: no tearing. Off: best on variable-refresh displays (ProMotion, G-Sync, FreeSync).");

    ImGui::Spacing();
    static const char *skip_names[] = { "Off", "Auto (when the game falls behind)", "Draw 1 of 2 frames",
                                        "Draw 1 of 3 frames", "Draw 1 of 4 frames" };
    int fs = g_settings.frameskip;
    if (ImGui::Combo("Frame skip", &fs, skip_names, IM_ARRAYSIZE(skip_names))) g_settings.frameskip = fs;
    hint("Skips drawing on slow machines so the game keeps its normal speed.");

    ImGui::Spacing();
    bool perf = g_settings.show_perf != 0;
    if (ImGui::Checkbox("Show performance", &perf)) g_settings.show_perf = perf;
}

static void tab_sound(void) {
    slider_pct("Music (BGM)", &g_settings.bgm);
    slider_pct("Sound effects (SFX)", &g_settings.sfx);
    slider_pct("Movies", &g_settings.movie);
    ImGui::Spacing();
    ImGui::TextDisabled("Mono / Stereo is in the game's own Options menu.");
}

static void tab_controls(void) {
    static const char *rows[][3] = {
        { "D-pad", "Arrow keys", "D-pad" }, { "Circle (confirm)", "X", "B" }, { "Cross (cancel)", "Z", "A" },
        { "Triangle", "S", "Y" }, { "Square", "A", "X" }, { "L1 / R1", "Q / E", "LB / RB" },
        { "L2 / R2", "1 / 3", "LT / RT" }, { "Start / Select", "Enter / Backspace", "Start / Back" },
        { "Fast-forward (hold)", "Tab", "" }, { "This window", "F2", "" },
    };
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("PS1"); ImGui::TableSetupColumn("Keyboard"); ImGui::TableSetupColumn("Controller");
        ImGui::TableHeadersRow();
        for (auto &r : rows) {
            ImGui::TableNextRow();
            for (int c = 0; c < 3; c++) { ImGui::TableSetColumnIndex(c); ImGui::TextUnformatted(r[c]); }
        }
        ImGui::EndTable();
    }
}

static void perf_overlay(void) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##perf", nullptr, fl)) {
        ImGui::Text("%.1f fps", g_stats.fps);
        ImGui::Text("frame work %.1f ms (max %.1f)", g_stats.busy_avg_ms, g_stats.busy_max_ms);
        ImGui::Text("skipped %d / s", g_stats.skipped_per_s);
    }
    ImGui::End();
}

extern "C" void ui_frame(void) {
    if (!s_ready || (!s_open && !g_settings.show_perf)) return;
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (g_settings.show_perf) perf_overlay();
    if (s_open) {
        ImGuiIO &io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
        bool keep = true;
        if (ImGui::Begin("Settings", &keep, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
            if (ImGui::BeginTabBar("tabs")) {
                static int want = -2;   /* debug: BRIG_UI_TAB=0/1/2 preselects a tab (screenshots) */
                if (want == -2) { const char *e = getenv("BRIG_UI_TAB"); want = e ? atoi(e) : -1; }
                auto sel = [&](int i) { ImGuiTabItemFlags f = want == i ? ImGuiTabItemFlags_SetSelected : 0; return f; };
                if (ImGui::BeginTabItem("Graphics", nullptr, sel(0))) { tab_graphics(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Sound", nullptr, sel(1))) { tab_sound(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Controls", nullptr, sel(2))) { tab_controls(); ImGui::EndTabItem(); }
                want = -1;
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            ImGui::TextDisabled("F2 / Esc to close. Settings are saved automatically.");
        }
        ImGui::End();
        if (!keep) close_ui();
    }
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s_ren);
}
