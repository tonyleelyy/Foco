cmake_minimum_required(VERSION 3.21)
project(Foco VERSION 0.1.5 LANGUAGES CXX RC)

add_executable(Foco WIN32
    src/main.cpp
    src/settings.cpp
    src/capture.cpp
    src/presentation.cpp
    src/foco.rc
)
target_compile_features(Foco PRIVATE cxx_std_20)
target_compile_definitions(Foco PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00)
target_link_libraries(Foco PRIVATE comctl32 d2d1 dwrite d3d11 dxgi dwmapi uxtheme user32 gdi32 msimg32
    ole32 oleacc shell32 uuid windowscodecs wtsapi32 advapi32 comdlg32)
if(MSVC)
    set_property(TARGET Foco PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(Foco PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8;/permissive->")
    target_link_options(Foco PRIVATE /MANIFEST:NO)
endif()

install(TARGETS Foco RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "Foco-${PROJECT_VERSION}-windows-x64")
include(CPack)
