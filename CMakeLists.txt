cmake_minimum_required(VERSION 3.20)
project(QuickBIOS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 必须在 add_executable 之前打开，且 mainwindow.h 必须进源列表 ────────────
# 漏掉 .h → AUTOMOC 不扫 Q_OBJECT → moc_mainwindow.cpp 不生成
#          → LNK2001 metaObject/qt_metacast/qt_metacall → LNK1120
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
# 保险：即使用户在 Kit / CMakeCache 里关过 AUTOMOC，这里也强制回来
set(CMAKE_AUTOMOC ON CACHE BOOL "" FORCE)

if(MSVC)
    # /utf-8：源码与执行字符集统一 UTF-8 → 治 mt.exe 乱码与中文注释告警
    # /EHsc：Qt 要求开 C++ 异常，否则 C2712/C4530 一串
    add_compile_options(/utf-8 /EHsc /W3)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /MD")
    set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG} /Od /MDd /RTC1")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)
# Qt5 用户改成： find_package(Qt5 REQUIRED COMPONENTS Widgets)

set(QUICKBIOS_SOURCES
    main.cpp
    mainwindow.h          # ← 必须在列表里，AUTOMOC 才认
    mainwindow.cpp
    mainwindow.ui
)

add_executable(QuickBIOS WIN32 ${QUICKBIOS_SOURCES})

target_link_libraries(QuickBIOS PRIVATE
    Qt6::Widgets
    advapi32              # OpenProcessToken / AdjustTokenPrivileges / Reg
    shell32               # ShellExecuteExW（自提权）
    user32                # ExitWindowsEx
)
# Qt5: Qt5::Widgets

# ══════════════ UAC 清单嵌入：根治 LNK1327 / LNK4075 ═══════════════════════
# 要点：
# ① quickbios.manifest 【不能】放进 add_executable 源列表（会被当源文件喂编译器）
# ② 链接器只给 /MANIFEST:NO；【严禁】再设 CMAKE_EXE_LINKER_FLAGS 里的
#    /MANIFESTFILE 或 /MANIFEST:EMBED —— 与 :NO 互斥，必报 LNK4075
# ③ mt.exe 用 CMake 自己探出来的绝对路径：Qt Creator 的 PATH 里通常没有
#    VS 的 Hostx64/x64 目录，直接写 mt.exe 就是
#    "The system cannot find the file specified" + 乱码 → LNK1327
if(MSVC AND WIN32)
    if(NOT CMAKE_MT)
        get_filename_component(_qb_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(CMAKE_MT mt.exe
            HINTS "${_qb_cl_dir}" "${_qb_cl_dir}/../../../.."
                  "${_qb_cl_dir}/../../../../x64"
            PATHS ENV PATH
            PATH_SUFFIXES x64 Hostx64/x64 bin
            NO_DEFAULT_PATH)
        if(NOT CMAKE_MT)
            find_program(CMAKE_MT mt.exe)     # 兜底：全 PATH 搜
        endif()
    endif()

    if(CMAKE_MT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/quickbios.manifest")
        # 让链接器别自己管清单
        set_target_properties(QuickBIOS PROPERTIES LINK_FLAGS "/MANIFEST:NO")
        add_custom_command(TARGET QuickBIOS POST_BUILD
            COMMAND "${CMAKE_MT}"
                    -manifest "${CMAKE_CURRENT_SOURCE_DIR}/quickbios.manifest"
                    -outputresource:"$<TARGET_FILE:QuickBIOS>;1"
            VERBATIM
            COMMENT "[QuickBIOS] 嵌入 UAC 清单 requireAdministrator")
    else()
        # 没找到 mt.exe 也不致命：main.cpp 的 ShellExecuteExW(runas) 会自提权
        message(WARNING
            "未找到 mt.exe 或 quickbios.manifest → 跳过静态 UAC 嵌入。\n"
            "  程序运行时仍会自行弹 UAC 提权；也可右键 exe『以管理员身份运行』。")
    endif()
endif()

# 顺手把 Qt DLL 拷到 exe 旁边（Release 下双击就能跑）
if(WIN32)
    add_custom_command(TARGET QuickBIOS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::Widgets>" "$<TARGET_FILE_DIR:QuickBIOS>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::Gui>"     "$<TARGET_FILE_DIR:QuickBIOS>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::Core>"    "$<TARGET_FILE_DIR:QuickBIOS>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:QuickBIOS>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
                "$<TARGET_FILE_DIR:QuickBIOS>/platforms/qwindows.dll"
        VERBATIM
        COMMENT "[QuickBIOS] 部署 Qt 运行时")
endif()
