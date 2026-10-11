# ============================================================================
#  tools/idf.ps1 —— 在本机激活 ESP-IDF v5.4.3 并执行 idf.py
# ============================================================================
#
#  【坑 1】为什么不能直接跑 ESP-IDF 自带的 export.ps1
#    D:\esp\v5.4.3\esp-idf\export.ps1 第 24 行是
#        $idf_exports = python "$idf_path/tools/activate.py" --export
#        . $idf_exports
#    而本机 PATH 上的 `python` 是 Microsoft Store 的占位程序
#    （C:\Users\Lenovo\AppData\Local\Microsoft\WindowsApps\python.exe），
#    它跑不了 activate.py，于是 $idf_exports 拿到空值，
#    第 27 行 `. $idf_exports` 抛 BadExpression，PATH 里拿不到 idf.py 与工具链，
#    表现为 `idf.py : 无法将"idf.py"项识别为 cmdlet...`。
#    → 解决：把 IDF 专用 python 放到 PATH 最前面，再点源 export.ps1。
#
#  【坑 2】为什么本文件刻意不用 param()
#    idf.py 自己的参数里有 -p / -b / -D 这些短选项。
#    如果本脚本声明了 param()，PowerShell 会先把 `build` 绑到第一个位置参数
#    （于是报"找不到 ESP-IDF：build\tools\idf.py"），再把 `-p` 当成
#    "未知参数"直接报错。
#    → 解决：不声明 param()，用 $args 原样转发，完全不干扰 idf.py 的参数。
#
#  【坑 3】本文件必须存成 UTF-8 with BOM
#    Windows PowerShell 5.1 读取「无 BOM 的 UTF-8」.ps1 时按 ANSI(GBK) 解码，
#    中文注释会全变乱码、字符串引号被吞掉，脚本直接语法错误。
#    改动本文件后请重新确认前 3 字节是 EF BB BF。
#
#  用法（在 D:\esp32\xiangmu1 目录下执行；脚本会自动切到 firmware/）：
#      .\tools\idf.ps1 build                          # 编译
#      .\tools\idf.ps1 -p COM4 flash                  # 烧录
#      .\tools\idf.ps1 -p COM4 monitor                # 看串口（Ctrl+] 退出）
#      .\tools\idf.ps1 -p COM4 flash monitor          # 烧完直接看串口
#      .\tools\idf.ps1 menuconfig                     # 图形化配置
#      .\tools\idf.ps1 fullclean                      # 彻底清理
#      . .\tools\idf.ps1                              # 只激活环境，不跑命令
#
#  被 Windows 执行策略拦住时（报"未对文件进行数字签名"）改用：
#      powershell -ExecutionPolicy Bypass -File .\tools\idf.ps1 build
#    或直接用包装脚本（推荐）：
#      .\tools\idf.cmd build
#
#  本机路径需要改动时不要改代码，用环境变量覆盖：
#      $env:IDF_SETUP_PATH  = 'D:\esp\v5.4.3\esp-idf'
#      $env:IDF_PY_ENV_PATH = 'C:\Espressif\python_env\idf5.4_py3.11_env'
# ============================================================================

$ErrorActionPreference = 'Stop'

# --- 0. 路径解析（故意用 $args 而不是 param()，原因见上方【坑 2】）---
$IdfPath = if ($env:IDF_SETUP_PATH) { $env:IDF_SETUP_PATH } else { 'D:\esp\v5.4.3\esp-idf' }
$IdfPythonEnv = if ($env:IDF_PY_ENV_PATH) { $env:IDF_PY_ENV_PATH } else { 'C:\Espressif\python_env\idf5.4_py3.11_env' }
$ProjectDir = Join-Path $PSScriptRoot '..\firmware'
$IdfArgs = @($args)

# --- 1. 校验 IDF 与它的 python 都在 ---
if (-not (Test-Path (Join-Path $IdfPath 'tools\idf.py'))) {
    throw "找不到 ESP-IDF：$IdfPath\tools\idf.py 不存在。请设置环境变量 IDF_SETUP_PATH。"
}

$pythonExe = Join-Path $IdfPythonEnv 'Scripts\python.exe'
if (-not (Test-Path $pythonExe)) {
    throw "找不到 IDF 的 python：$pythonExe 不存在。请设置环境变量 IDF_PY_ENV_PATH。"
}

# --- 2. 关键一步：IDF 的 python 必须排在 PATH 最前面（见【坑 1】）---
$env:PATH = (Join-Path $IdfPythonEnv 'Scripts') + ';' + $env:PATH
$env:IDF_PATH = $IdfPath
$env:IDF_PYTHON_ENV_PATH = $IdfPythonEnv

# --- 3. 点源官方 export.ps1，把工具链 / cmake / ninja 挂到 PATH ---
# 【坑 4】activate.py 会往 stderr 打一行 "Activating ESP-IDF 5.4"。
#   若把 stderr 并进管道（2>&1）而 $ErrorActionPreference 又是 Stop，
#   PowerShell 会把这条 native stderr 当成 ErrorRecord 直接终止整个脚本
#   （报 NativeCommandError，位置指向 export.ps1:24，且之后没有任何输出）。
#   → 解决：临时把错误策略放回 Continue，并把全部输出流转到 $null。
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    . (Join-Path $IdfPath 'export.ps1') *> $null
}
finally {
    $ErrorActionPreference = $prevEap
}

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "export.ps1 执行后 PATH 里仍然没有 idf.py。请把完整报错发给开发者。"
}

# --- 4. 没有额外参数：脚本到此为止，只激活环境 ---
if ($IdfArgs.Count -eq 0) {
    Write-Host "[idf.ps1] ESP-IDF 环境已激活（IDF_PATH=$IdfPath）" -ForegroundColor Green
    Write-Host "[idf.ps1] 现在可以直接用 idf.py；工程目录：$ProjectDir" -ForegroundColor Green
    return
}

# --- 5. 有参数：切到 firmware 目录并执行 idf.py ---
if (-not (Test-Path $ProjectDir)) {
    throw "工程目录不存在：$ProjectDir"
}

Push-Location $ProjectDir
try {
    Write-Host "[idf.ps1] > idf.py $($IdfArgs -join ' ')" -ForegroundColor Cyan
    # idf.py 只是 python 的包装，编译出错时 CMake 会往 stderr 打大段文字。
    # 在 $ErrorActionPreference='Stop' 下这些 stderr 会被当成 terminating error
    # 截断掉真正的报错内容，所以这里同样临时放行。
    $ErrorActionPreference = 'Continue'
    & idf.py @IdfArgs
    $code = $LASTEXITCODE
}
finally {
    Pop-Location
}

exit $code
