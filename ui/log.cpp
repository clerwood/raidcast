#include "log.h"

#include <windows.h>

#include <imgui.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace raidcast {
namespace {

constexpr std::size_t kMaxLines = 2000;

std::mutex&             Mutex() { static std::mutex m; return m; }
std::deque<std::string>& Lines() { static std::deque<std::string> l; return l; }

bool g_console_attached = false;
bool g_scroll_pending   = false;

void Append(const char* prefix, const std::string& text) {
    std::string line = prefix ? std::string(prefix) + text : text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

    {
        std::lock_guard<std::mutex> lock(Mutex());
        Lines().push_back(line);
        while (Lines().size() > kMaxLines) Lines().pop_front();
        g_scroll_pending = true;
    }

    // Still goes to the terminal when there is one; --headless depends on it.
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
}

std::string Format(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n <= 0) return {};

    std::string out(static_cast<std::size_t>(n), '\0');
    std::vsnprintf(out.data(), static_cast<std::size_t>(n) + 1, fmt, args);
    return out;
}

}  // namespace

bool AttachParentConsole() {
    // If stdout is already somewhere real - redirected to a file or pipe, or an
    // inherited console handle - write there and do not touch the console.
    // Attaching would replace a redirection and break scripted runs.
    const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing != nullptr && existing != INVALID_HANDLE_VALUE) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        g_console_attached = true;
        return true;
    }

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;

    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_console_attached = true;
    return true;
}

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const std::string text = Format(fmt, args);
    va_end(args);
    Append(nullptr, text);
}

void LogError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const std::string text = Format(fmt, args);
    va_end(args);
    Append("! ", text);
}

void FatalError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const std::string text = Format(fmt, args);
    va_end(args);
    Append("! ", text);

    MessageBoxA(nullptr, text.c_str(), "RaidCast", MB_ICONERROR | MB_OK);
}

std::vector<std::string> LogLines() {
    std::lock_guard<std::mutex> lock(Mutex());
    return {Lines().begin(), Lines().end()};
}

std::string LogText() {
    std::lock_guard<std::mutex> lock(Mutex());
    std::string out;
    for (const auto& l : Lines()) {
        out += l;
        out += "\r\n";  // clipboard text on Windows
    }
    return out;
}

void DrawLogPanel(const char* label, bool* open, float height) {
    ImGui::SetNextItemOpen(*open, ImGuiCond_Always);
    if (!ImGui::CollapsingHeader(label)) {
        *open = false;
        return;
    }
    *open = true;

    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(LogText().c_str());

    ImGui::BeginChild("##loglines", ImVec2(0, height), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    bool scroll = false;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        scroll = g_scroll_pending;
        g_scroll_pending = false;
        for (const auto& line : Lines()) {
            const bool is_error = line.rfind("! ", 0) == 0;
            if (is_error) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1.0f));
            ImGui::TextUnformatted(line.c_str());
            if (is_error) ImGui::PopStyleColor();
        }
    }
    // Follow the tail only while the user is already at the bottom, so reading
    // back through the log is not yanked away by new lines.
    if (scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

}  // namespace raidcast
