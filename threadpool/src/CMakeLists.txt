# src/CMakeLists.txt
# 把 src 里实现做成一个库，然后链接到可执行文件
# 改动要点：
#  - 先 find_package(yaml-cpp REQUIRED)
#  - 把 yaml-cpp 链接到 core（PUBLIC），这样 app 链接 core 时会继承 yaml-cpp
#  - 使用 安全检查：如果 fetch/import 的目标名为 yaml-cpp::yaml-cpp 则使用它，
#    否则回退到传统的 yaml-cpp（兼容不同导出名）

# 列出源文件（显式列举比 glob 更可控）
set(CORE_SOURCES
    autothread.cpp
    main.cpp
    taskqueue.cpp
    utility.cpp
    workbranch.cpp
    workspace.cpp
    supervisor.cpp
)

# 先查找 yaml-cpp（因为 core 的实现使用到 YAML::LoadFile）
find_package(yaml-cpp REQUIRED)

add_library(core ${CORE_SOURCES})

# 公开头文件所在路径（使用 target_include_directories）
target_include_directories(core
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>  # 安装时头文件放的位置
)

# 要求 C++17
target_compile_features(core PUBLIC cxx_std_17)

# 将 yaml-cpp 链接到 core（使用现代导出目标名 yaml-cpp::yaml-cpp 如果存在）
if (TARGET yaml-cpp::yaml-cpp)
    target_link_libraries(core PUBLIC yaml-cpp::yaml-cpp)
else()
    # 备选：有些 yaml-cpp 打包/版本可能导出不同目标名或只提供 yaml-cpp
    target_link_libraries(core PUBLIC yaml-cpp)
endif()

# 可执行文件（假设 main.cpp 在 src 同目录）
add_executable(app main.cpp)

# app 依赖 core；由于 core 已经 PUBLIC 链接 yaml-cpp，app 无需显式再次链接 yaml-cpp
target_link_libraries(app PRIVATE core)

# 如果你想添加安装规则：
install(TARGETS core app
        EXPORT MyAppTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
