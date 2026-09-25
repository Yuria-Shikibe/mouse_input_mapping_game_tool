# 贡献指南

欢迎提交问题和 Pull Request。提交内容按项目 MIT 许可证提供；第三方文件继续遵循其原始许可。

## 开发流程

1. 安装 Visual Studio C++ x64 工具链、Windows SDK、CMake 3.24+ 和 Ninja。
2. 在 x64 Native Tools 命令行运行 `cmake --preset debug`、`cmake --build --preset debug`。
3. 修改后运行 `ctest --preset debug`，发布前也运行 Release 构建和测试。
4. 说明问题、行为变化和验证结果。涉及输入状态或配置逻辑时添加对应回归测试。

运行测试前关闭正在运行的本工具；运行时集成测试会检查全局单实例约束，不能与实际程序或另一组集成测试同时运行。

源码位于 `src/`，测试位于 `tests/`，项目资源位于 `assets/`。
不要提交 `build/`、`dist/`、IDE 缓存、实际使用的 `config.ini` 或本地工具链路径。
不要直接修改上游代码或二进制；更新依赖时同时更新来源、版本、校验值和许可材料。

## 问题反馈

请提供程序版本、Windows 版本、复现步骤、预期与实际结果，以及必要的错误输出。
涉及键盘鼠标时注明设备类型，配置内容请移除私人路径。
自动测试使用模拟接口，不证明真实驱动或游戏兼容性；实际输入问题请参考 README 的验收步骤。

## 发布

按 README 构建、测试并通过 CPack 生成发布包。版本号统一修改根目录 `CMakeLists.txt`。
发布完整 ZIP，保留 LICENSE、第三方声明、许可和用户态库源码，不打包个人配置。
GitHub Actions 在推送和 PR 时验证 Debug/Release，并提供 Release ZIP 工件，不会自动发布 Release。
