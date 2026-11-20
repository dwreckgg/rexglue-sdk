# Third-party notices

This SDK (BSD 3-Clause, see LICENSE) builds on and bundles the following. Each library's full licence is in its folder
under thirdparty/ (RenderDoc's at the top of thirdparty/renderdoc/renderdoc_app.h).

- Xenia (https://github.com/xenia-project/xenia): BSD 3-Clause, Copyright (c) 2022, Ben Vanik and Xenia project
  contributors. Many runtime files are derived from Xenia (see each file's header); the RenderDoc attach probe follows
  Xenia Canary's renderdoc_api.cc.
- Upstream ReXGlue (https://github.com/rexglue/rexglue-sdk): BSD 3-Clause, Copyright (c) 2026, Tom Clay; the SDL
  window, Wayland surface, runtime video driver and text-input commits are cherry-picked from it (the last three
  co-authored by Marco Andronaco).
- SDL3 (thirdparty/sdl3): zlib licence, Copyright (C) 1997-2026 Sam Lantinga.
- Dear ImGui (thirdparty/imgui): MIT, Copyright (c) 2014-2025 Omar Cornut.
- Tracy (thirdparty/tracy): BSD 3-Clause, Copyright (c) 2017-2025, Bartosz Taudul.
- utfcpp (thirdparty/utfcpp): Boost Software License 1.0.
- RenderDoc API header (thirdparty/renderdoc): MIT, Copyright (c) 2015-2016 Baldur Karlsson.
- On Linux, the window layer links the system's wayland-client, libX11 and libxcb (MIT/X11 licences).
