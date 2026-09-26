# Mouse Input Mapping

Windows x64 终端工具，提供两个独立 target / EXE：

| target / EXE 名称 | 输入方案 | 映射范围 | 默认配置文件 |
| --- | --- | --- | --- |
| `mouse_input_mapping_kernel` | Interception 内核过滤驱动 | X- → A，X+ → D；过滤 X、保留 Y | `config.ini` |
| `mouse_input_mapping_user` | Raw Input + SendInput，无第三方驱动 | XY 四向 + 鼠标五键 + 滚轮滚动转键盘 | `config.user.ini` |

两个版本均默认 OFF、F8 切换、Ctrl+C 退出，不允许同时运行。内核版本保持原有功能和配置兼容性，原 `mouse_input_mapping` 构建 target 已重命名为 `mouse_input_mapping_kernel`。

## Steam 启动选项：随游戏运行

先单独启动对应版本完成键位配置；内核版还需安装驱动并按提示重启。之后将 Steam 游戏的“启动选项”设为以下其中一项，并把工具路径替换为实际绝对路径：

```text
"C:\tools\mouse_input_mapping_user.exe" --daemon %command%
"C:\tools\mouse_input_mapping_kernel.exe" --daemon %command%
```

如需指定配置，在 `--daemon` 前加入 `--config "C:\path\my.ini"`。`--daemon` 后的第一个参数必须是实际游戏 EXE，后续参数原样转发给游戏。工具启动游戏后记录 PID；游戏进程结束时记录退出事件、释放映射按键并退出。工具提前退出不会关闭游戏。只跟踪直接启动的进程：若传入的程序只是启动器，它退出时工具也会退出，即使它另行启动的游戏还在运行。

daemon 模式要求配置文件存在，且本版本使用的所有键位都已在文件中明确绑定；缺少或无效时直接报错，不启动游戏。内核版还要求驱动已可用，此模式不会安装驱动或弹出配置向导。两个版本仍默认 OFF，可用 F8 切换。游戏继承 Steam 的环境和工作目录；游戏进程与工具控制台分离，关闭工具控制台不会向游戏发送控制台关闭事件。

作者：**mo_yanxi**。项目原创代码采用 [MIT License](LICENSE)，第三方依赖见 [许可声明](THIRD_PARTY_NOTICES.md)。

## 用户态版本：XY 四向映射

运行 `mouse_input_mapping_user.exe`，首次依次录入左、右、上（Y-）、下（Y+）、五个鼠标按钮、滚轮上滑/下滑的目标键和切换键。滚轮上滑与下滑可以映射到同一个键；其余映射目标键必须互不相同。按下目标键盘按键后用 Enter 确认；IDE 控制台可以输入键名。新配置的默认值采用 `cmake-build-debug/bin/config.user.ini` 中的键位和参数；程序仍读取 EXE 同目录的 `config.user.ini`。

| 输入 | 配置字段 | 默认输出 |
| --- | --- | --- |
| X- / X+ | `left_key` / `right_key` | A / D |
| Y- / Y+ | `up_key` / `down_key` | S / W |
| 左键 LMB | `lmb_key` | SUBTRACT |
| 右键 RMB | `rmb_key` | M |
| 滚轮按下 CMB | `cmb_key` | T |
| 侧键 1 / 侧键 2 | `x1_key` / `x2_key` | LSHIFT / F |
| 滚轮上滑 / 下滑 | `wheel_up_key` / `wheel_down_key` | R / R |
| 开关 | `toggle_key` | F8 |

```powershell
.\mouse_input_mapping_user.exe
.\mouse_input_mapping_user.exe --configure
.\mouse_input_mapping_user.exe --configure-text
.\mouse_input_mapping_user.exe --config C:\my_config\mouse.user.ini
```

游戏内需关闭鼠标输入，并把对应动作绑定到表中的目标键盘按键。用户态程序监听鼠标并输出键盘按键，不屏蔽原始 XY 或鼠标按钮。斜向移动时 X/Y 独立输出各自方向键，并独立处理抖动、反向与超时释放；两轴共用 `window_ms`、`start_counts`、`reverse_counts`、`release_ms` 参数。参考 `config.user.example.ini`。

XY 可分别选择长按或脉冲：默认 X 持续按住方向键，Y 使用脉冲。配置向导可输入 `0` 或 `1` 启用各轴脉冲，并输入 `0～1` 浮点数设置单次按住比例。配置文件字段为 `x_pulse_enabled`、`y_pulse_enabled`、`x_hold_ratio`、`y_hold_ratio`、`pulse_period_ms`。默认周期 30 ms，Y 比例为 `0.65`。鼠标位移越快脉冲越密，最多积压一次脉冲；高速输入超过输出能力时会饱和。比例 `0` 不输出该轴按键，比例 `1` 仍保留至少 1 ms 松开间隔。内核版只使用 X 字段；启用 X 脉冲后仍过滤原始 X。

`x_keyboard_override_enabled` 默认为 `1`。开启后，只要实体键盘按住 `left_key` 或 `right_key` 中任意一个，整个鼠标 X 轴的合成按键输出就会暂停，实体键盘输入优先；最后一个实体左右键松开后，如果鼠标方向仍在有效时间内，则立即恢复对应的鼠标映射输出。设置为 `0` 可恢复旧版的实体键与鼠标映射合并行为。此选项只影响 X 轴，不影响 Y、鼠标按钮或滚轮映射。

鼠标五键直接映射：按下发目标键 DOWN，松开发 UP，支持同时按住多个按钮；按钮不受 XY 的超时释放影响。滚轮上滑、下滑每刻度分别发送目标键约 10 ms 的短按，两次之间至少释放 10 ms；最多排队八次，避免大量滚动造成长时间滞后。高分辨率滚轮的不足一刻度位移会按设备累计，拔掉设备时清除。滚轮按下是独立的中键 CMB，侧键指标准 XBUTTON1 / XBUTTON2。多只鼠标的同一按钮按住状态会合并，拔掉设备会释放其按钮占用。关闭映射或正常退出释放映射持有的键。启用前需先松开鼠标按钮和所有映射目标键。水平滚轮暂不映射。

此 EXE 可单独运行，不加载或释放 Interception DLL、不安装驱动，不需要首次管理员授权或重启。它也不会卸载以前由内核版本安装的驱动。没有 `--install-driver` 或驱动 `--check` 选项。Windows 权限级别和游戏规则仍可能限制 SendInput；无驱动不代表游戏一定接受或允许该映射。

切换键由用户态低级键盘钩子处理；开启时十一个映射目标键也经该钩子维护实体与自动按住状态。X 左右键默认采用键盘优先互斥，其余目标键仍合并所有权，避免自动松键打断实体按住。用户态钩子不区分实体键盘，因此不保证多个键盘同时按住同一映射键的合并；其拦截也不保证能阻止游戏自己的 Raw Input 键盘路径。OFF 时普通键盘输入透传。强制结束进程或系统终止不能保证松键清理。

支持相对鼠标，多只鼠标共同参与映射；绝对定位设备只处理按钮，不映射坐标。验证时检查四个方向、斜向组合、独立超时、反向先松后按、五个按钮按住/松开与组合、关闭时松键，并在目标游戏内确认按键生效。用户态不应通过 `raw_input_probe --expect-zero-x` 验收，因为它有意保留原始鼠标数据。

## 内核态版本：安装和使用

`mouse_input_mapping_kernel.exe` 已内置 Interception DLL 和官方驱动安装器，可以单独复制运行，无需额外下载依赖或安装 VC++ Redistributable。完整发布包还包含 Raw Input 检测工具、说明和许可文件。

1. 直接运行 EXE。首次录入左移键、右移键和快捷键：在普通 Windows 终端中，按下目标键，看到如 `Selected: F8 (0x0042)` 的回显后，按 Enter 确认。直接按 Enter 保留当前显示的键位，默认 `A`、`D`、`F8`。

   ```powershell
   .\mouse_input_mapping_kernel.exe
   ```

2. 如果系统缺少驱动，程序会自动启动内置安装器，由 Windows 请求管理员授权。允许后完成安装，程序提示重启；配置已保存，**不会自动重启**。如果取消授权，下次运行可以再试。

3. 首次安装后重启 Windows，再运行同一个 EXE，即可使用原配置，默认 **OFF**。驱动已经可用时，无需再次安装或授权。

4. `F8` 全局切换 ON/OFF。成功开启时播放 Windows“设备连接”音效，关闭时播放“设备断开”音效，跟随系统声音方案和系统声音音量；异步播放，不阻塞输入处理。在程序终端按 `Ctrl+C` 退出。关闭映射会立即释放映射持有的键。开关按键不会传给游戏；长按不会连续切换或重复播放音效。开启前先松开两个映射键。

游戏原始输入的 X 轴过滤需要系统驱动。因此“单 EXE 自包含、自动部署”可以实现，但首次管理员授权和重启仍是必要步骤；不能等价替换为免驱动方案。驱动已注册但暂不可用时会提示重启，不会每次重复安装；重启后仍不可用，需检查系统是否阻止了驱动。

**CLion 等 IDE 的输出控制台**可能不支持直接捕获 F1–F24，程序会自动使用名称输入模式：键入 `F8` 这两个字符并回车，看到名称回显后再次回车确认；不要在该模式下直接按物理 F8。也可使用 `--configure-text` 主动选择这种模式。

配置操作：

```powershell
.\mouse_input_mapping_kernel.exe --configure
.\mouse_input_mapping_kernel.exe --configure-text
.\mouse_input_mapping_kernel.exe --config C:\my_config\mouse.ini
.\mouse_input_mapping_kernel.exe --check
.\mouse_input_mapping_kernel.exe --install-driver
.\mouse_input_mapping_kernel.exe --help
```

`--configure` / `--configure-text` 确认并保存后退出，不安装或要求驱动。文本模式可输入新名称反复修改，空行确认；未确认就结束输入不会覆盖配置。`--check` 仅检查状态，不安装驱动。`--install-driver` 显式安装/修复驱动，成功时返回 Windows 的“需要重启”状态码 3010；常规启动自动安装完成后的退出码为 0。默认配置是 EXE 同目录的 `config.ini`，不取决于当前工作目录；目录不可写时使用 `--config`。已有配置会直接加载；重新绑定请使用 `--configure`。旧版扫描码配置完全兼容，新保存的配置会附带键名注释。

旧版本若在显示键位后报 `Cannot enable driver input filters`，请使用修复后的 EXE。本机已确认驱动过滤查询可能成功却不返回数据，旧版将其误判为启用失败；新版已移除该回读校验。若 `--check` 已通过，此误报无需重装驱动。

名称输入支持 `A`–`Z`、`0`–`9`、`F1`–`F24`、`LEFT/RIGHT/UP/DOWN`、`SPACE/ENTER/NUM_ENTER/TAB/ESC/BACKSPACE`、`INSERT/DELETE/HOME/END/PAGE_UP/PAGE_DOWN`、`LSHIFT/RSHIFT/LCTRL/RCTRL/LALT/RALT`、`LWIN/RWIN/APPS`、`CAPS_LOCK/NUM_LOCK/SCROLL_LOCK`、`NUM0`–`NUM9`、`ADD/SUBTRACT/MULTIPLY/DIVIDE/DECIMAL`。也可输入 `0x001e` 或 `0xe04b` 形式的物理扫描码。名称不区分大小写，按当前键盘布局解析后保存物理键位。要绑定 Enter 本身，使用 `--configure-text` 输入 `ENTER`；直接按键模式中的 Enter 用于确认。组合键、Pause 和 PrintScreen 不支持。三个键位必须互不相同。

## 调整手感

参考源码中的 `assets/config.example.ini`（发布包中为 `config.example.ini`）。按键配置和参数均在 `[mapping]` 下；省略的字段采用默认值。

| 参数 | 默认值 | 作用 |
| --- | ---: | --- |
| `window_ms` | 30 | 净横向位移累计窗口，过期数据丢弃 |
| `start_counts` | 3 | 从空闲开始按住、同方向续按所需的净位移 |
| `reverse_counts` | 6 | 切换到反方向需要的净位移 |
| `release_ms` | 60 | 最后一次确认方向后，多久没有确认就松键 |

位移单位为鼠标原始 counts，与 DPI 有关，不是屏幕像素。增大 `start_counts` 可以压制小幅抖动；减小会更灵敏。增大 `reverse_counts` 可减少误反向；减小会更快转向。减小 `release_ms` 可以更快停下，但缓慢移动时可能断续。`release_ms` 必须不少于 `window_ms`，`reverse_counts` 必须不少于 `start_counts`。

每次达到阈值后清空过滤累计量；未确认的小幅抖动、零位移和纯纵向移动不会刷新方向时间。反向时先松开原映射键再按下新键。未启用脉冲的轴持续同向移动只保持按下；启用脉冲的轴按确认位移发出短按和释放。

脉冲模式使用 `max(window_ms, 2 × release_ms, 120 ms)` 的净位移累计窗口（默认 120 ms），让连续低速微动也能达到阈值，正反位移仍互相抵消。达到 `start_counts`（默认 3 counts）确认方向后即可产生一次脉冲，无需再累计到 10 counts；若上一脉冲尚未完成，则等待按住和松开间隔结束。反向仍需达到 `reverse_counts`。单次 1 count 的微动在默认设置下不会触发。

方向仍在 `release_ms` 内没有新确认时超时，并清除待发脉冲；尚未过期的未确认位移保留，只有收到新的非零位移时才会再次确认方向。停止移动不会自动重复发键，但已确认的待发脉冲可在方向超时前输出。关闭映射会清空全部累计状态。长按模式继续使用原有的 `window_ms` 窗口与超时清理规则。

运行时所有受 Interception 管理的相对鼠标一起参与映射。键盘输出使用开启映射时触发快捷键的键盘设备。默认启用键盘优先互斥：实体左/右映射键按住期间暂停鼠标 X 合成输出，自动松键不会释放仍被用户实际按住的同一键；用户同时实际按下两个方向键时，仍保留两个实体输入。关闭互斥选项后恢复实体与自动映射合并的旧行为。

## 验证真实输入

另开终端运行：

```powershell
.\raw_input_probe.exe
```

该程序创建不可见的消息窗口，仅接收和报告 Raw Input，不拦截输入。先开启映射，然后按 `F9` 清零计数，再左右、上下及斜向移动鼠标。开启期间应看到：

- `nonzero_x_packets=0`，即没有任何含非零 X 的相对鼠标包；不能只看可能正负抵消的 `sum_x`。
- 纵向移动时 `sum_y` 改变；点击和滚轮的计数正常增加。
- 键盘日志显示配置键的 DOWN/UP；停止后出现 UP，反向时旧键 UP 在新键 DOWN 之前。
- 关闭映射后横向输入恢复。`F9` 是检测工具的计数重置键，验收时不要把它配置为映射或切换键。

也可在映射已经开启时运行定时检查，并在期间实际移动鼠标：

```powershell
.\raw_input_probe.exe --seconds 10 --expect-zero-x
```

检测到非零 X、没有收到任何相对鼠标包、或读取失败都会返回非零退出码。仍需人工确认 Y、按钮、滚轮和键盘事件。检测工具会在终端打印按键扫描码，验收结束后关闭即可。

## 构建和测试

需要 Windows x64、Visual Studio 的 C++ 工具链、Windows SDK、CMake 3.24+。在 x64 Native Tools 命令行中运行（Ninja 生成器另需 Ninja）：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/release
cpack --config build/release/CPackConfig.cmake -C Release
```

仅构建某一方案可用 `cmake --build --preset release --target mouse_input_mapping_user` 或 `--target mouse_input_mapping_kernel`。用户态 target 不编译或链接驱动部署代码，不嵌入第三方资源。

编译程序位于 `build/release/bin/`，安装目录为 `dist/windows-x64/`，版本化 ZIP 位于 `dist/`。CPack 从安装规则生成干净发布包，不读取已有安装目录，因此不会混入本地 `config.ini`。Debug 开发可将 preset 换为 `debug`。发布时使用完整 ZIP，以保留许可材料。

也可以在 CLion 中直接打开 CMake 项目并选择 MSVC x64。第三方文件已固定版本，无构建时下载。测试不启用真实驱动过滤，覆盖净位移/抖动/反向/超时、8 kHz 输入序列、按键状态合并、发送失败、扫描码和配置持久化。集成测试使用单独目录里的模拟 DLL，验证鼠标包、开关、超时释放及驱动读取失败后的清理；该 DLL 不会安装或打包进交付目录。真实驱动与游戏效果必须按上面的步骤实测。

## 项目结构

```text
assets/       配置示例、Windows 资源模板（构建输入）
src/          程序源码和内部头文件
tools/        Raw Input 检测工具
tests/        单元测试、模拟驱动和集成测试
third_party/  固定版本依赖、源码及原始许可（构建输入）
cmake/        发布打包规则
docs/         验证记录
.github/      Windows Release 构建、测试及 ZIP 上传 CI
build/        编译输出、生成资源及测试临时文件（不提交）
dist/         最终安装目录和发布 ZIP（不提交）
```

第三方 DLL 和安装器是固定的构建输入，因此保留在 `third_party/`；它们不依赖 `dist/`。贡献方式见 [CONTRIBUTING.md](CONTRIBUTING.md)，验证范围见 [docs/VALIDATION.md](docs/VALIDATION.md)。

## 兼容性与卸载

Interception 官方仅声明测试到 Windows 10。Windows 11、本机系统构建号 26200 和具体游戏的兼容性需要实测。`--check` 仅确认 DLL、驱动和设备可访问，不证明实际过滤或游戏兼容性。程序在正常启动且驱动未注册时自动发起管理员安装，不切换为效果较弱的鼠标钩子，不自动重启或修改系统安全设置。

未提供外部 `interception.dll` 时，从 EXE 资源释放到随机命名、限定访问权限的临时目录，使用完自动删除。仍允许替换同目录 DLL，以保留第三方库的可替换性。管理员安装只执行 EXE 内置的官方安装器，不执行工作目录中的同名文件。Release 与 IDE 的 Debug 构建都内置相同依赖。

支持范围为驱动管理的相对鼠标及标准键盘输入路径。绝对定位设备（例如部分触控板、数位板、远程输入）保持原样并提示；绕过该输入路径的设备/API、反作弊对驱动的限制、特殊游戏对键盘输入的拒绝不在兼容保证内。首版不提供游戏自动切换或指定单一鼠标。

正常退出和可处理错误会尝试释放映射按键、关闭过滤。强制结束进程、驱动错误、设备拔出或系统崩溃不能保证完成按键清理。不要在映射按住时拔出用于输出的键盘；出现残留键状态时，恢复键盘后实际按下再松开对应键。程序关闭后再卸载驱动：

```powershell
# 管理员终端执行，完成后重启。
.\driver\install-interception.exe /uninstall
```

Interception 上游来源、版本、SHA-256 和原始许可文件位于 `third_party/interception/`。该依赖区分非商业与商业许可，商业使用前请查阅其条款。
