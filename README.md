<p align="center">
  <img src="assets/icon.png" width="96" height="96" alt="DesktopIconToggle icon">
</p>

# 桌面图标开关 / DesktopIconToggle

一个很小的 Windows 托盘工具：**双击桌面空白处**即可显示或隐藏桌面图标。

**下载**：[最新版 Release](https://github.com/Fantasy724/DesktopIconToggle/releases/latest)

- 单文件、免安装、无需管理员权限
- 原生 Win32 程序，64 位 EXE 约 67 KB
- 不联网、不收集数据、不写日志、不驻留服务
- 只在用户明确勾选时写入开机启动项（HKCU）
- 退出时自动恢复由它隐藏的桌面图标
- 附带完整源代码，可自行审查和编译

## 使用方法

1. 双击 `DesktopIconToggle.exe`，程序在任务栏托盘区运行（图标是一个蓝色眼睛）。
2. 在桌面空白处双击：隐藏桌面图标；再次双击：显示桌面图标。
3. 右键托盘图标，可以：
   - 显示 / 隐藏桌面图标
   - 开启 / 关闭“开机自动启动”
   - 查看使用说明
   - 退出
4. 退出时，如果图标是由本程序隐藏的，会自动恢复显示。

> 双击桌面上的图标本身不会触发切换，程序会通过 Windows 辅助功能接口判断落点是否在图标上。

> 升级版本前，请先从托盘菜单退出正在运行的旧版本。程序是单实例的；旧版还在运行时，新版会直接退出，看起来像“没有反应”。

## 安全软件提示（重要）

本程序没有数字签名。部分安全软件（尤其是开启了“入侵防御 / 应用程序控制”的卡巴斯基）会**阻止未签名程序访问桌面、读取鼠标状态或创建托盘图标**。表现可能是：

- 双击后没有任何反应；
- 托盘里没有图标；
- 弹出“无法创建托盘图标”或“无法切换桌面图标”的提示。

遇到这种情况，请先把本程序加入安全软件的信任/排除列表，再重新运行：

1. 打开卡巴斯基 → 设置 → 安全 → 入侵防御 / 应用程序控制 → 管理应用程序；
2. 找到 `DesktopIconToggle.exe`，将其设为“信任”；
3. 再在“设置 → 安全 → 排除项”中把该文件或所在文件夹加入排除；
4. 退出本程序（如果还在运行），重新双击运行。

其他安全软件（360、火绒等）也可能有类似的“未知程序拦截”，处理方式相同。

本程序在开发机上通过了 **卡巴斯基 21.26 文件扫描：1 个对象 OK，0 个检测**。但“扫描不报毒”不等于安全软件一定会放行它的桌面操作，必要时仍需手动信任。

## 兼容性

- Windows 10 / Windows 11，64 位
- 支持多显示器、高 DPI
- 不依赖 .NET、VC++ 运行库或任何第三方 DLL
- 仅使用 Windows 自带的 `user32.dll`、`shell32.dll`、`ole32.dll`、`oleacc.dll` 等系统组件

## 卸载

1. 右键托盘图标 → 退出（会自动恢复桌面图标）。
2. 如果开过“开机自动启动”，先在托盘菜单里取消勾选。
3. 删除 `DesktopIconToggle.exe` 即可。

程序只在以下位置留下可选的启动项：

```
HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run
    值名：DesktopIconToggle
```

取消勾选“开机自动启动”会删除该值。

## 从源码构建

需要 MinGW-w64（`g++.exe`、`windres.exe`、`strip.exe`）。本仓库开发时使用 w64devkit。

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

输出：

```
dist\DesktopIconToggle.exe
```

如果编译器不在 PATH，可把 w64devkit 放到项目的 `..\tools\w64devkit\` 或 `tools\w64devkit\`，构建脚本会自动查找。

## 源码结构

```
src/main.cpp       主程序（Win32 / 托盘 / 双击检测 / MSAA 图标判断）
src/app.rc         图标、清单、版本信息等资源
src/app.manifest   DPI 感知、asInvoker、Windows 10/11 兼容性
src/app.ico        程序图标
src/make_icon.py   重新生成图标的脚本（需要 Pillow）
assets/icon.png    README 使用的程序图标
build.ps1          构建脚本
```

## 实现说明

- 每 16 ms 轮询一次鼠标左键状态，不安装全局鼠标钩子，避免侵入其他程序。
- 用 `GetDoubleClickTime` 和系统双击距离判断双击，尊重用户的系统设置。
- 用 `WindowFromPoint` 找到落点，再用 `GetAncestor(GA_ROOT)` 确认顶层窗口是桌面宿主 `Progman` / `WorkerW`。文件资源管理器文件夹窗口（`CabinetWClass`）即使内部也有 `SHELLDLL_DefView` / `SysListView32`，也不会被误判为桌面。
- 用 `AccessibleObjectFromPoint` 判断落点是否在某个桌面图标上；如果无法读取辅助功能信息，则不会贸然切换，而是提示用户。
- 用 `ShowWindow` 显示/隐藏桌面图标列表窗口，即时生效，不需要重启 Explorer。
- 不联网、不注入进程、不修改系统文件、不安装驱动或服务。

## 已知限制

- 如果 Explorer 重启，桌面图标会恢复显示；本程序会在托盘重建后继续工作。
- 极短的鼠标点击（按下到抬起少于约 16 ms）可能不会被轮询捕捉；正常人工双击不受影响。
- 如果强行结束进程，已隐藏的图标不会自动恢复；请通过托盘菜单正常退出。
- 安全软件的行为拦截需要用户手动放行，这是程序外部因素。

## 更新记录

- **1.0.1**：修复在文件资源管理器文件夹空白处双击也会切换桌面图标的问题。现在只认顶层窗口为 `Progman` / `WorkerW` 的桌面，文件夹窗口（`CabinetWClass`）不会再被误判。
- **1.0.0**：初始版本。

## 许可

MIT License，见 `LICENSE`。
