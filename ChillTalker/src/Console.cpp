#include "Console.h"

#include <windows.h>

#include <conio.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <iostream>

namespace con {

static HANDLE            g_out         = INVALID_HANDLE_VALUE;
static bool              g_outConsole  = false;
static bool              g_inConsole   = false;
static WORD              g_defaultAttr = 0;
static std::string       g_live;          // prompt + typed text while a prompt is up, else empty
static std::atomic<bool> g_quit{false};

static BOOL WINAPI CtrlHandler(DWORD type)
{
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
    g_quit = true;
    return TRUE;
  }
  return FALSE;
}

void Init()
{
  g_out = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_SCREEN_BUFFER_INFO info;
  g_outConsole = GetConsoleScreenBufferInfo(g_out, &info) != 0;
  if (g_outConsole) g_defaultAttr = info.wAttributes;

  DWORD mode = 0;
  g_inConsole = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;

  SetConsoleCtrlHandler(CtrlHandler, TRUE);
}

bool Quit()
{
  return g_quit;
}

bool EscPressed()
{
  bool esc = false;
  while (g_inConsole && _kbhit()) {
    const int c = _getch();
    if (c == 0 || c == 0xE0) _getch();    // arrow or function key: second code follows
    else if (c == 27)        esc = true;
  }
  return esc;
}

static WORD AttrFor(Color color)
{
  const WORD keep = WORD(g_defaultAttr & 0xF0);   // keep the background
  switch (color) {
    case Color::Tx:    return WORD(keep | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    case Color::Rx:    return WORD(keep | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    case Color::Warn:  return WORD(keep | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    case Color::Error: return WORD(keep | FOREGROUND_RED | FOREGROUND_INTENSITY);
    default:           return g_defaultAttr;
  }
}

static void SetColor(Color color)
{
  std::fflush(stdout);
  if (g_outConsole) SetConsoleTextAttribute(g_out, AttrFor(color));
}

void Print(Color color, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  std::string text(size_t(n > 0 ? n : 0), '\0');
  va_start(ap, fmt);
  std::vsnprintf(&text[0], text.size() + 1, fmt, ap);
  va_end(ap);

  if (!g_live.empty()) std::printf("\r%*s\r", int(g_live.size()), "");
  SetColor(color);
  std::fputs(text.c_str(), stdout);
  SetColor(Color::Normal);
  std::fputc('\n', stdout);
  if (!g_live.empty()) std::fputs(g_live.c_str(), stdout);
  std::fflush(stdout);
}

bool ReadLine(const std::string& prompt, std::string& line, const std::function<void()>& idle)
{
  line.clear();

  if (!g_inConsole) {
    std::fputs(prompt.c_str(), stdout);
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::printf("%s\n", line.c_str());   // echo it, so a scripted run reads like a session
    return true;
  }

  g_live = prompt;
  std::fputs(prompt.c_str(), stdout);
  std::fflush(stdout);

  while (!g_quit) {
    if (!_kbhit()) {
      idle();
      Sleep(10);
      continue;
    }

    const int c = _getch();
    if (c == 0 || c == 0xE0) {            // arrow or function key: second code follows
      _getch();
    } else if (c == '\r' || c == '\n') {
      line = g_live.substr(prompt.size());
      g_live.clear();
      std::fputc('\n', stdout);
      std::fflush(stdout);
      return true;
    } else if (c == 8 || c == 127) {      // backspace
      if (g_live.size() > prompt.size()) {
        g_live.pop_back();
        std::fputs("\b \b", stdout);
      }
    } else if (c == 27) {                 // Esc clears what was typed
      while (g_live.size() > prompt.size()) {
        g_live.pop_back();
        std::fputs("\b \b", stdout);
      }
    } else if (c >= 32 && c < 127) {
      g_live.push_back(char(c));
      std::fputc(c, stdout);
    }
    std::fflush(stdout);
  }

  g_live.clear();
  std::fputc('\n', stdout);
  return false;
}

}  // namespace con
