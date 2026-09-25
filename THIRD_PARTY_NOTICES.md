# 第三方组件

本项目原创代码及文档由 mo_yanxi 以 [MIT License](LICENSE) 发布。
MIT 许可证不覆盖 `third_party/interception/` 中的上游代码、DLL、驱动安装器和许可文件，
也不改变嵌入可执行文件中的第三方组件的许可。

## Interception 1.0.1

- 作者：Francisco Lopes。
- 上游：<https://github.com/oblitum/Interception>
- 固定版本、来源及校验值：[依赖说明](third_party/interception/README.md)。
- 原始条款：[许可目录](third_party/interception/licenses/)。
- 用户态库源码：[source](third_party/interception/source/)。

上游声明：非商业用途的库及源码采用 LGPL，相关驱动和安装器的分发需通过库及其 API
与驱动通信；商业用途另有许可。具体使用和分发请遵循上游原始条款。
本项目的 MIT 许可不会授予第三方组件的商业许可。驱动及安装器源码未包含在上游公开仓库中。

程序动态加载 Interception，支持将替换版本的 `interception.dll` 放在 EXE 同目录。
完整发布包保留用户态库源码、原始许可和本说明；再分发内置依赖的 EXE 时也应一并保留这些材料。
