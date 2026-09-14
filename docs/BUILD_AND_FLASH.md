# 构建、合并与烧录

## 1. 准备依赖

在仓库根目录运行：

```powershell
.\scripts\prepare_private_components.ps1 `
    -SecureHex '<PSOC_Edge_Hello_World>\build\project_hex\proj_cm33_s_signed.hex' `
    -AcceptInfineonEula
```

仓库已经包含共享 BSP 和 RT-Thread 源码。脚本从官方 Infineon 仓库固定到
`emUSB-Host release-v2.3.0`。`-AcceptInfineonEula` 表示运行者已经阅读并接受
随软件提供的 Infineon EULA；脚本不会替用户改变许可证。

## 2. 导入 RT-Thread Studio

选择 `File > Import > Existing Projects into Workspace`，分别选择：

```text
<REPOSITORY>\cm33
<REPOSITORY>\cm55
```

导入后名称必须分别为：

```text
AI_Kit_RTThread_Base
AI_Kit_RTThread_M55_Demo
```

不要复制到 workspace；让 Studio 直接引用 Git 工作区，避免出现两个不同版本。

关闭 RT-Thread Studio GUI 后才可以在命令行使用 `eclipsec.exe` 做隔离的
Managed Build；同一工程不要同时由 GUI 与 headless Eclipse 打开。工程元数据已
使用 `${eclipse_home}` 和 `${workspace_loc}`，不得重新写入某台电脑的盘符。

## 3. SCons Release

```powershell
$StudioRoot = '<RT_STUDIO_ROOT>'
$RepoRoot = '<REPOSITORY>'
$Scons = Join-Path $StudioRoot 'platform\env_released\env-new\.venv\Scripts\scons.exe'
$Toolchain = Join-Path $StudioRoot 'repo\Extract\ToolChain_Support_Packages\ARM\GNU_Tools_for_ARM_Embedded_Processors\13.3\bin'
$env:RTT_EXEC_PATH = $Toolchain

Set-Location (Join-Path $RepoRoot 'cm33')
& $Scons -j12 BUILD=release
if ($LASTEXITCODE -ne 0) { throw 'M33 Release failed' }

Set-Location (Join-Path $RepoRoot 'cm55')
& $Scons -j12 BUILD=release
if ($LASTEXITCODE -ne 0) { throw 'M55 Release failed' }
```

## 4. 合并三镜像

```powershell
$EdgeProtect = Join-Path $RepoRoot 'cm33\tools\edgeprotecttools\bin\edgeprotecttools.exe'
Set-Location (Join-Path $RepoRoot 'cm55')
& $EdgeProtect run-config -i firmware_bundle.json
if ($LASTEXITCODE -ne 0) { throw 'Firmware bundle failed' }
```

输入为：

```text
cm33/build/rtthread_app.hex
cm33/tools/secure/proj_cm33_s_signed.hex
cm55/rtthread.hex
```

输出为：

```text
cm55/build/ai_kit_rtthread_cm33_cm55.hex
```

## 5. 烧录

烧录必须使用完整三镜像 HEX。连接 KitProg3 后，在仓库根目录执行：

```powershell
.\scripts\flash.ps1 -StudioRoot '<RT_STUDIO_ROOT>'
```

脚本使用 OpenOCD 的 `target/infineon/pse84xgxs2.cfg` 和
`interface/kitprog3.cfg`，完成写入、校验和复位。等价的手工命令如下：
示例（把占位符替换为本机 RT-Thread Studio 根目录）：

```powershell
Set-Location '<RT_STUDIO_ROOT>\repo\Extract\Debugger_Support_Packages\Infineon\OpenOCD-Infineon\2.0.0\bin'
.\openocd.exe -s ../scripts -s ../flm/cypress/cat1d `
    -f interface/kitprog3.cfg -f target/infineon/pse84xgxs2.cfg `
    -c 'program <REPOSITORY>/cm55/build/ai_kit_rtthread_cm33_cm55.hex verify reset exit'
```

烧录成功必须看到 `Programming Finished` 或 `Verified OK`，随后在串口和屏幕上
验收 M33、M55、显示触摸及所需应用。

## 6. 发布验证

公开推送前运行：

```powershell
.\scripts\check_public_release.ps1
```

该检查只审查 Git 将要提交的文件，不把本地恢复的受限依赖误判为公开内容。

## 7. 两条构建链的职责

- **SCons Release** 是正式发布和三镜像合并的权威构建链。
- **RT-Studio Managed Debug** 用于 IDE 日常增量编译和调试，M33、M55 都必须能
  产生 `Debug/rtthread.elf` 与 `Debug/rtthread.hex`。
- `.config`、`rtconfig.h`、SCons、`.cproject` 必须同步。修改 RT-Thread Settings
  后先检查差异，再分别验证这两条构建链。
- M55 的 `makefile.targets` 只删除顶层生成目录，避免 Windows 命令行因逐个列出
  数千个对象文件而触发 Error 87。
- 完整烧录永远使用 `cm55/build/ai_kit_rtthread_cm33_cm55.hex`，不能用任一单核
  `rtthread.hex` 替代。
