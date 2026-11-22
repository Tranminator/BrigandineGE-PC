// Settings window (F2): tabs for Graphics, Sound, Controls, Save and System, built with Dear ImGui.
// Changes apply immediately; closing the window (F2 / Esc / Close) saves settings.cfg.
#include "ui.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "video_gl.h"
#include "gpu_hw.h"
#include "mouse.h"
#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include "settings.h"
#include "state.h"
void platform_apply_video(void);
}
#include <time.h>

static SDL_Window *s_win;
static bool s_open;
static bool s_ready;
static char s_toast[128];
static Uint32 s_toast_until;

static const char *FONT_CANDIDATES[] = {
    "/System/Library/Fonts/Supplemental/Arial.ttf",      // macOS
    "/System/Library/Fonts/Helvetica.ttc",
    "C:\\Windows\\Fonts\\segoeui.ttf",                   // Windows
    "C:\\Windows\\Fonts\\arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   // Linux
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
};

extern "C" void ui_init(SDL_Window *win, void *gl_context) {
    s_win = win;
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
    vgl_drawable_size(&ow, &oh);
    float dpi = ww ? (float)ow / ww : 1.0f;
    for (const char *path : FONT_CANDIDATES) {
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fclose(f);
        if (io.Fonts->AddFontFromFileTTF(path, 17.0f * dpi)) { io.FontGlobalScale = 1.0f / dpi; break; }
    }
    ImGui_ImplSDL2_InitForOpenGL(win, gl_context);
    ImGui_ImplOpenGL3_Init("#version 150");
    s_ready = true;
}

extern "C" int ui_is_open(void) { return s_open; }

extern "C" void ui_toast(const char *msg) {
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toast_until = SDL_GetTicks() + 2000;
}

static void toast_overlay(void) {
    if (!s_toast[0] || SDL_GetTicks() > s_toast_until) return;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12, io.DisplaySize.y - 12), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##toast", nullptr, fl)) ImGui::TextUnformatted(s_toast);
    ImGui::End();
}

static void close_ui(void) { s_open = false; settings_save(); vgl_save_config(); }

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

    ImGui::SeparatorText("Renderer");
    {
        static const char *names[] = { "Native (software, 320x240)", "2x", "3x", "4x", "5x", "6x", "7x", "8x" };
        int maxs = hw_max_scale(), cur_s = g_settings.render_scale < 1 ? 1 : g_settings.render_scale;
        if (ImGui::BeginCombo("Internal resolution", names[cur_s - 1])) {
            for (int s = 1; s <= maxs && s <= 8; s++)
                if (ImGui::Selectable(names[s - 1], s == cur_s)) { g_settings.render_scale = s; platform_apply_video(); }
            ImGui::EndCombo();
        }
        hint("2x and up draw the game with the graphics card at a higher resolution: sharper 3D, smooth "
             "colour gradients. Native is the exact PS1 picture. Scanline/CRT shaders look best at Native.");
    }

    ImGui::SeparatorText("Shader");
    int cur = vgl_shader_current();
    if (ImGui::BeginCombo("Shader", vgl_shader_name(cur))) {
        for (int i = 0; i < vgl_shader_count(); i++) {
            if (i > 0 && vgl_shader_is_builtin(i - 1) && !vgl_shader_is_builtin(i)) ImGui::Separator();
            if (ImGui::Selectable(vgl_shader_name(i), i == cur)) vgl_select_shader(i);
            if (i == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) vgl_rescan_shaders();
    static const char *descs[] = {
        "Crisp pixels without uneven widths at any window size (default).",
        "Raw nearest-neighbour pixels.",
        "Bilinear filtering: soft image.",
        "Dark gaps between lines, like a TV.",
        "Curved CRT screen: scanlines, shadow mask, glow.",
    };
    cur = vgl_shader_current();
    if (cur >= 0 && cur < (int)IM_ARRAYSIZE(descs) && vgl_shader_is_builtin(cur)) hint(descs[cur]);
    else {
        bool lin = vgl_filter_linear() != 0;
        if (ImGui::Checkbox("Bilinear texture filter", &lin)) vgl_set_filter_linear(lin);
    }
    for (int i = 0; i < vgl_param_count(); i++) {
        VglParam *p = vgl_param(i);
        ImGui::PushID(i);
        if (ImGui::SliderFloat(p->desc, &p->value, p->min, p->max, "%.2f")) {
            if (p->step > 0) p->value = p->min + (float)(int)((p->value - p->min) / p->step + 0.5f) * p->step;
            vgl_param_changed(i);
        }
        ImGui::PopID();
    }
    if (vgl_param_count() > 0 && ImGui::Button("Reset to defaults")) vgl_reset_params();
    hint("More shaders: put RetroArch single-pass .glsl files in the shaders folder, then press Reload.");
    if (ImGui::Button("Open shaders folder")) {
        char url[1200]; snprintf(url, sizeof url, "file://%s", vgl_shader_dir());
        SDL_OpenURL(url);
    }

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
        { "Fast-forward (hold)", "Tab", "" }, { "Quick save / load", "F5 / F9", "" }, { "Change save slot", "Left Shift + Left / Right", "" },
        { "This window", "F2", "" },
    };
    bool m = g_settings.mouse != 0;
    if (ImGui::Checkbox("Mouse control", &m)) g_settings.mouse = m;
    ImGui::BeginDisabled(!m);
    bool cap = g_settings.mouse_capture != 0;
    if (ImGui::Checkbox("Game pointer (captures the mouse)", &cap)) g_settings.mouse_capture = cap;
    ImGui::BeginDisabled(!cap);
    ImGui::SliderInt("Pointer speed", &g_settings.mouse_speed, 5, 100, "%d%%");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    hint("The mouse drives the game's own cursor (one pointer). Battle map: move to a hex, click to select, push "
         "against a screen edge to scroll (keeps scrolling at the game's Cursor Speed until you move back); attack / spell targets the same way. Strategy map: the quill follows the "
         "mouse, click a castle; in troop lists Ctrl / Shift / middle click marks a troop. Menus, Organize grids and lists (attacks, magic, "
         "skills, SUMMON...): move up / down, click to choose, push past the end or use the wheel to scroll. Yes / No "
         "boxes: move left / right, click to answer. Unit status: point at an attack / spell / skill to read it. Right click = Cross "
         "(back), middle click = Triangle, wheel = Up / Down. Start = right click on a map (free cursor) or a side button. "
         "DOMAIN: the box follows the mouse, click to jump there. Title: click for PUSH START, then point and click. SELECT RULER: push sideways or use the wheel for the next ruler. SAVE / LOAD: point at a file, click. UNITS: push the mouse sideways for the other columns. The mouse is released while this window is open or "
         "the game is in the background.");
    ImGui::Spacing();
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

static void tab_system(void) {
    static const char *names[] = { "Original CD speed", "Fast" };
    int fl = g_settings.fast_load;
    if (ImGui::Combo("Loading", &fl, names, IM_ARRAYSIZE(names))) g_settings.fast_load = fl;
    hint("Original: disc reads take as long as on a PS1 (2x CD drive), so loading screens such as the "
         "castle name and \"Now Loading\" stay up like on the console. Fast: data loads instantly.");
}

/* ---------------------------------------------------------------- Save tab */
#define NSLOTS (10 + AUTO_COUNT)
struct SlotView { int slot; int status; StateHeader h; unsigned tex; };
static SlotView *s_slots;          /* index 0..9 manual, 10.. autosaves */
static int s_slots_version = -1;

static int slot_of(int i) { return i < 10 ? i + 1 : AUTO_BASE + (i - 9); }

static void refresh_slots(void) {
    if (!s_slots) s_slots = (SlotView *)calloc(NSLOTS, sizeof(SlotView));
    s_slots_version = state_files_version();
    for (int i = 0; i < NSLOTS; i++) {
        SlotView &v = s_slots[i];
        v.slot = slot_of(i);
        v.status = state_read_info(v.slot, &v.h);
        if (v.status != 1) continue;
        if (!v.tex) v.tex = vgl_create_rgb_texture(THUMB_W, THUMB_H, v.h.thumb);
        else vgl_update_rgb_texture(v.tex, THUMB_W, THUMB_H, v.h.thumb);
    }
}

static void slot_info(const SlotView &v, char *line1, char *line2, size_t n) {
    if (v.status == 0) { snprintf(line1, n, "Empty"); line2[0] = 0; return; }
    if (v.status < 0) { snprintf(line1, n, "Made by another version of the port"); line2[0] = 0; return; }
    time_t t = (time_t)v.h.time;
    struct tm *lt = localtime(&t);
    char when[64] = "";
    if (lt) strftime(when, sizeof when, "%Y-%m-%d  %H:%M", lt);
    snprintf(line1, n, "%s", when);
    if (!v.h.in_game) snprintf(line2, n, "Title / menu");
    else snprintf(line2, n, "Year %u, Month %u  -  %s", v.h.year, v.h.month,
                  v.h.battle ? "Battle" : v.h.phase ? "Attack phase" : "Organize phase");
}

static void tab_save(void) {
    if (s_slots_version != state_files_version() || !s_slots) refresh_slots();
    static int confirm_slot = 0, confirm_kind = 0;    /* 1 overwrite, 2 delete */

    bool as = g_settings.autosave != 0;
    if (ImGui::Checkbox("Autosave", &as)) { g_settings.autosave = as; state_reset_autosave_tracking(); }
    ImGui::SameLine();
    ImGui::TextDisabled("at each new phase and when a battle starts (3 rotating slots)");
    hint("F5 saves to the selected slot, F9 loads it. Click a slot to select it, or press Left Shift + Left / Right in game. "
         "Memory card saves in the game's own menu still work and stay compatible with emulators.");

    const float th = 72.0f, tw = th * THUMB_W / THUMB_H;
    ImGui::BeginChild("slots", ImVec2(0, 380), ImGuiChildFlags_Borders);
    for (int i = 0; i < NSLOTS; i++) {
        SlotView &v = s_slots[i];
        bool manual = v.slot <= 10;
        if (i == 10) { ImGui::Spacing(); ImGui::SeparatorText("Autosaves"); }
        ImGui::PushID(v.slot);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool selected = manual && v.slot == state_slot();
        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, th)) && manual)
            state_set_slot(v.slot);
        ImGui::SetCursorScreenPos(p0);
        if (v.status == 1 && v.tex) ImGui::Image((ImTextureID)(intptr_t)v.tex, ImVec2(tw, th));
        else {
            ImVec2 a = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRect(a, ImVec2(a.x + tw, a.y + th), ImGui::GetColorU32(ImGuiCol_Border), 3.0f);
            ImGui::Dummy(ImVec2(tw, th));
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        char name[32], l1[96], l2[96];
        state_slot_name(v.slot, name, sizeof name);
        slot_info(v, l1, l2, sizeof l1);
        ImGui::TextUnformatted(name);
        ImGui::TextDisabled("%s", l1);
        if (l2[0]) ImGui::TextUnformatted(l2);
        ImGui::EndGroup();

        float bw = 64.0f, right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(right - bw * 3 - 16);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, th * 0.5f - ImGui::GetFrameHeight() * 0.5f - 4));
        if (manual) {
            if (ImGui::Button("Save", ImVec2(bw, 0))) {
                state_set_slot(v.slot);
                if (v.status == 0) state_request_save(v.slot);
                else { confirm_slot = v.slot; confirm_kind = 1; ImGui::OpenPopup("confirm"); }
            }
        } else ImGui::Dummy(ImVec2(bw, 0));
        ImGui::SameLine();
        ImGui::BeginDisabled(v.status != 1);
        if (ImGui::Button("Load", ImVec2(bw, 0))) {
            if (manual) state_set_slot(v.slot);
            state_request_load(v.slot);
            close_ui();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(v.status == 0);
        if (ImGui::Button("Delete", ImVec2(bw, 0))) { confirm_slot = v.slot; confirm_kind = 2; ImGui::OpenPopup("confirm"); }
        ImGui::EndDisabled();
        ImGui::EndGroup();

        if (ImGui::BeginPopupModal("confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            char nm[32]; state_slot_name(confirm_slot, nm, sizeof nm);
            ImGui::Text(confirm_kind == 1 ? "Overwrite %s?" : "Delete %s?", nm);
            if (ImGui::Button(confirm_kind == 1 ? "Overwrite" : "Delete", ImVec2(110, 0))) {
                if (confirm_kind == 1) state_request_save(confirm_slot); else state_request_delete(confirm_slot);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

/* the game pointer (mouse captured): a small arrow with an outline */
static void cursor_overlay(float x, float y) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float s = 1.0f;
    ImVec2 p[7] = { { x, y }, { x, y + 17 * s }, { x + 4.5f * s, y + 13 * s }, { x + 7.5f * s, y + 19.5f * s },
                    { x + 10 * s, y + 18.5f * s }, { x + 7 * s, y + 12 * s }, { x + 12.5f * s, y + 12 * s } };
    dl->AddConcavePolyFilled(p, 7, IM_COL32(255, 255, 255, 255));
    dl->AddPolyline(p, 7, IM_COL32(0, 0, 0, 255), ImDrawFlags_Closed, 1.5f);
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
    bool toast = s_toast[0] && SDL_GetTicks() <= s_toast_until;
    float cx, cy;
    bool cursor = mouse_cursor_pos(&cx, &cy) != 0;
    if (!s_ready || (!s_open && !g_settings.show_perf && !toast && !cursor)) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (g_settings.show_perf) perf_overlay();
    toast_overlay();
    if (cursor) cursor_overlay(cx, cy);
    if (s_open) {
        ImGuiIO &io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Always);
        bool keep = true;
        if (ImGui::Begin("Settings", &keep, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
            if (ImGui::BeginTabBar("tabs")) {
                static int want = -2;   /* debug: BRIG_UI_TAB=0/1/2 preselects a tab (screenshots) */
                if (want == -2) { const char *e = getenv("BRIG_UI_TAB"); want = e ? atoi(e) : -1; }
                auto sel = [&](int i) { ImGuiTabItemFlags f = want == i ? ImGuiTabItemFlags_SetSelected : 0; return f; };
                if (ImGui::BeginTabItem("Graphics", nullptr, sel(0))) { tab_graphics(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Sound", nullptr, sel(1))) { tab_sound(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Controls", nullptr, sel(2))) { tab_controls(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Save", nullptr, sel(3))) { tab_save(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("System", nullptr, sel(4))) { tab_system(); ImGui::EndTabItem(); }
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
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
