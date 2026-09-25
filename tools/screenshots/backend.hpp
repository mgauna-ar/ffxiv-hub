#pragma once

#include <string>

namespace shots {

class Frame;

/// The folder GetWindowsDirectoryA reports. The app and the payload read Segoe UI
/// from its Fonts subfolder.
void set_windows_dir(std::string dir);
[[nodiscard]] const std::string& windows_dir();

/// Where ImGui_ImplDX11_RenderDrawData and present() copy the frame they are handed.
/// Null discards it, as for the frames that only let a layout settle.
void set_capture(Frame* frame);

/// What a platform and renderer backend do before ImGui::NewFrame() on the current
/// context: a display of `width` x `height`, a 60 Hz tick, no mouse over the
/// window, and the font atlas uploaded once.
void prepare_frame(float width, float height);

/// Hands the current context's last ImGui::Render() to the capture.
void present();

} // namespace shots
