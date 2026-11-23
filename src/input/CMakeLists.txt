# rexinput - Input system library
# Input system and drivers for Xbox 360 controller emulation

add_library(rexinput OBJECT
    input_system.cpp
    pad_remap.cpp
    pad_refresher.cpp
    pad_vibration.cpp
    mnk/mnk_input_driver.cpp
    nop/nop_input_driver.cpp
)
add_library(rex::input ALIAS rexinput)

target_include_directories(rexinput PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(WIN32)
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp sdl/sdl_pad_trace.cpp xinput/xinput_input_driver.cpp)
else()
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp sdl/sdl_pad_trace.cpp)
endif()

target_link_libraries(rexinput PUBLIC rexcore rexui SDL3::SDL3)
