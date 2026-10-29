// Console output and a line editor that keeps the screen tidy while the serial port
// is being serviced: anything printed while a prompt is waiting wipes the prompt line,
// prints, and then puts the prompt and the text typed so far back.
//
// Works on a redirected stdin too (plain line reads, no polling), so a session can be
// scripted.
#pragma once

#include <functional>
#include <string>

namespace con {

enum class Color { Normal, Tx, Rx, Warn, Error };

void Init();

// True once Ctrl+C was pressed.
bool Quit();

// True when Esc was pressed since the last call; other keys typed meanwhile are dropped.
// Always false when stdin is not a console.
bool EscPressed();

// Prints one line (the newline is added).
void Print(Color color, const char* fmt, ...);

// Reads one line after showing the prompt. While no key is pressed, idle() is called
// about every 10 ms. Returns false at end of input or after Ctrl+C.
bool ReadLine(const std::string& prompt, std::string& line, const std::function<void()>& idle);

}  // namespace con
