#include "editor_files.h"
#include <stdexcept>
#include <cwchar>
#ifdef _WIN32
#include <SDL_syswm.h>
#include <windows.h>
#include <commdlg.h>
#endif

namespace emberframe::lab::editor {
std::optional<std::filesystem::path> choose_file(SDL_Window* window,FileKind kind) {
#ifdef _WIN32
    SDL_SysWMinfo info{};SDL_VERSION(&info.version);
    if(!SDL_GetWindowWMInfo(window,&info))throw std::runtime_error(SDL_GetError());
    // Windows 筛选项按“名称\0通配符\0”成对排列，末尾用双零结束。
    // “所有文件”只放宽选择器显示，不改变模型导入器实际支持的格式。
    const wchar_t* filter=kind==FileKind::model?L"模型 (GLB / glTF / OBJ)\0*.glb;*.gltf;*.obj\0所有文件 (*.*)\0*.*\0\0":
        kind==FileKind::texture?L"纹理 (PNG / JPG / TGA / BMP)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0\0":L"EmberFrame 工程\0*.ember\0\0";
    std::wstring path(32768,L'\0');OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=info.info.win.window;ofn.lpstrFilter=filter;ofn.lpstrFile=path.data();ofn.nMaxFile=DWORD(path.size());
    ofn.nFilterIndex=1; // 默认仍选第一项；追加模型时可在下拉框切换“所有文件”。
    const bool save=kind==FileKind::save_project;
    ofn.lpstrTitle=save?L"另存为 EmberFrame 工程":nullptr;ofn.lpstrDefExt=save?L"ember":nullptr;
    ofn.Flags=OFN_EXPLORER|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if(save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn)){path.resize(std::wcslen(path.c_str()));return std::filesystem::path(path);}
    if(const DWORD error=CommDlgExtendedError())throw std::runtime_error("File dialog error: "+std::to_string(error));
    return {};
#else
    (void)window;(void)kind;
    throw std::runtime_error("System file picker unavailable on this platform; use the path field or drag and drop");
#endif
}
}
