param(
    [switch]$r,        # -r for Release build
    [switch]$c,        # -c to ONLY clean, without building
    [switch]$rebuild,  # -rebuild to force a clean and build
    [switch]$v         # -v for Verbose deployment logging
)

# --- Console Hygiene ---
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if ($v) { $VerbosePreference = 'Continue' }
$ESC = [char]27; $c_green = "${ESC}[92m"; $c_white = "${ESC}[97m"; $c_gray = "${ESC}[90m"; $c_red = "${ESC}[91m"; $c_yellow = "${ESC}[93m"; $c_cyan = "${ESC}[96m"; $c_bold = "${ESC}[1m"; $c_dim = "${ESC}[2m"; $c_reset = "${ESC}[0m";
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch {}
try { $PSStyle.OutputRendering = if ($env:WT_SESSION) { 'Ansi' } else { 'Host' } } catch {}
$env:DOTNET_CLI_UI_LANGUAGE = 'en'; $env:VSLANG = '1033'; $env:MSBUILDDISABLENODEREUSE = '1'; $env:CLICOLOR_FORCE = '1'
$prevCursor = $Host.UI.RawUI.CursorSize; $Host.UI.RawUI.CursorSize = 0
$origFG = $Host.UI.RawUI.ForegroundColor; $origBG = $Host.UI.RawUI.BackgroundColor

try {
    # --- Configuration ---
    $ProjectRoot = (Get-Location).Path
    
    if (-not (Test-Path (Join-Path $ProjectRoot "src"))) {
        throw "Build script must be run from the root of the project directory (the folder containing 'src')."
    }

    $SourceDir   = Join-Path $ProjectRoot "src"
    $BuildDir    = Join-Path $ProjectRoot "src" "build"
    $BuildType   = if ($r) { "Release" } else { "Debug" }
    
    $VcpkgToolchainFile = "A:/vcpkg/scripts/buildsystems/vcpkg.cmake"
    
    function Clean-Build {
        Write-Host "--- Cleaning build directory... ---" -ForegroundColor Yellow
        if (Test-Path $BuildDir) { Remove-Item -Path $BuildDir -Recurse -Force -EA SilentlyContinue }
        if (Test-Path (Join-Path $ProjectRoot "CSO")) { Remove-Item -Path (Join-Path $ProjectRoot "CSO") -Recurse -Force -EA SilentlyContinue }
        Get-ChildItem -Path $ProjectRoot -Include @("*.exe", "*.dll", "*.pdb", "*.pyd") -File | Where-Object { 
            $_.Name -notin @("DirectML.dll", "onnxruntime.dll", "onnxruntime_providers_shared.dll") 
        } | Remove-Item -Force -EA SilentlyContinue
        Write-Host "Clean complete."
    }

    # --- Main Build Logic ---
    Write-Host "Project Universal Build System 💪 (Incremental)" -ForegroundColor Green
    Write-Host "Project Root set to: '$ProjectRoot'" -ForegroundColor DarkGray
    
    if ($c) { Clean-Build; exit }
    if ($rebuild) { Clean-Build }

    Write-Host "--- Configuring CMake Project ($BuildType)... ---" -ForegroundColor Green
    & cmake -S $SourceDir -B $BuildDir -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=$BuildType -DCMAKE_TOOLCHAIN_FILE="$VcpkgToolchainFile"
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

    Write-Host "--- Executing CMake Build ($BuildType)... ---" -ForegroundColor Cyan
    & cmake --build $BuildDir --config $BuildType -- /m
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }

    # --- Deployment Section ---
    Write-Host "--- Deploying CMake artifacts... ---" -ForegroundColor Cyan
    
    $deployedExecutables = [System.Collections.ArrayList]::new()
    $deployedDlls = [System.Collections.ArrayList]::new()
    $deployedPyModules = [System.Collections.ArrayList]::new()

    # 1. Deploy Executables
    Write-Host "  Deploying ALL custom executables to Root..." -ForegroundColor Yellow
    if (Test-Path $BuildDir) {
        $excludedExeNames = @('RUN_TESTS', 'install', 'gtest', 'gmock')
        
        $allArtifacts = Get-ChildItem -Path $BuildDir -Recurse -File -Include "*.exe"
        
        foreach ($artifact in $allArtifacts) {
            Write-Verbose "Considering: $($artifact.FullName)"

            # Filter 1: Is the artifact's immediate parent directory named 'Debug' or 'Release'?
            if ($artifact.Directory.Name -ne $BuildType) {
                Write-Verbose "  -> SKIPPED: Parent folder '$($artifact.Directory.Name)' is not '$BuildType'."
                continue
            }

            # Filter 2: Is the artifact's name in our exclusion list?
            if ($excludedExeNames -contains $artifact.BaseName) {
                Write-Verbose "  -> SKIPPED: Name '$($artifact.BaseName)' is in the exclusion list."
                continue
            }
            
            Write-Verbose "  -> DEPLOYING..."
            Copy-Item -Path $artifact.FullName -Destination $ProjectRoot -Force
            if (-not $deployedExecutables.Contains($artifact.Name)) {
                $deployedExecutables.Add($artifact.Name) | Out-Null
            }
        }
    } else {
        Write-Host "    [WARNING] Build directory '$BuildDir' not found. Nothing to deploy." -ForegroundColor Yellow
    }
    
    # 2. Deploy Python Modules
    Write-Host "  Deploying Python Modules (.pyd) to Root..." -ForegroundColor Yellow
    Get-ChildItem -Path $BuildDir -Recurse -File -Include "*.pyd" | ForEach-Object {
        Copy-Item -Path $_.FullName -Destination $ProjectRoot -Force
        if (-not $deployedPyModules.Contains($_.Name)) {
            $deployedPyModules.Add($_.Name) | Out-Null
        }
    }

    # 3. Deploy necessary Runtime DLLs
    Write-Host "  Deploying Runtime DLLs to Root..." -ForegroundColor Yellow
    $VendorLibDir = Join-Path $SourceDir "vendor/onnxruntime/lib"
    if (Test-Path $VendorLibDir) {
        Get-ChildItem -Path $VendorLibDir -File -Include @("onnxruntime.dll", "onnxruntime_providers_shared.dll", "DirectML.dll") | ForEach-Object {
            Copy-Item -Path $_.FullName -Destination $ProjectRoot -Force -ErrorAction SilentlyContinue
            if (-not $deployedDlls.Contains($_.Name)) {
                $deployedDlls.Add($_.Name) | Out-Null
            }
        }
    }

    # 4. Deploy Compiled Shaders
    Write-Host "  Deploying Shaders to /CSO..." -ForegroundColor Yellow
    $ShaderSourceDir = Join-Path $BuildDir "Shaders"
    $ShaderDestDir = Join-Path $ProjectRoot "CSO"
    if (Test-Path $ShaderSourceDir) {
        if (-not (Test-Path $ShaderDestDir)) {
            New-Item -Path $ShaderDestDir -ItemType Directory | Out-Null
        }
        Copy-Item -Path "$ShaderSourceDir\*.cso" -Destination $ShaderDestDir -Force -ErrorAction SilentlyContinue
        $shaderCount = (Get-ChildItem -Path $ShaderDestDir -File -ErrorAction SilentlyContinue).Count
        if ($shaderCount -gt 0) {
            Write-Host "    [DEPLOYED] Copied $shaderCount shaders to '$ShaderDestDir'" -ForegroundColor Green
        }
    }
    
    Write-Host ""
    Write-Host "--- Deployment Summary ---" -ForegroundColor Cyan
    Write-Host "Executables moved to '$ProjectRoot':" -ForegroundColor White
    if ($deployedExecutables.Count -gt 0) {
        $deployedExecutables | ForEach-Object { Write-Host "  - $_" -ForegroundColor Green }
    } else {
        Write-Host "  (None)" -ForegroundColor Gray
        Write-Host "  If this is unexpected, run the build again with the '-v' flag for details." -ForegroundColor Yellow
    }
    
    Write-Host "Python Modules moved to '$ProjectRoot':" -ForegroundColor White
    if ($deployedPyModules.Count -gt 0) {
        $deployedPyModules | ForEach-Object { Write-Host "  - $_" -ForegroundColor Green }
    } else {
        Write-Host "  (None)" -ForegroundColor Gray
    }

    Write-Host "DLLs moved to '$ProjectRoot':" -ForegroundColor White
    $uniqueDlls = $deployedDlls | Sort-Object -Unique
    if ($uniqueDlls.Count -gt 0) {
        $uniqueDlls | ForEach-Object { Write-Host "  - $_" -ForegroundColor Green }
    } else {
        Write-Host "  (None)" -ForegroundColor Gray
    }
    Write-Host ""
    
    Write-Host "--- Build and Deployment Successful! ---" -ForegroundColor Green

} catch {
    Write-Host ""; Write-Host "${c_bold}${c_red}--- BUILD FAILED ---${c_reset}"
    Write-Host "${c_red}$($_.Exception.Message)${c_reset}"
    $callStack = $_.ScriptStackTrace -split "`n" | Select-Object -Skip 1
    if ($callStack) { Write-Host "${c_dim}$($callStack -join "`n")${c_reset}" }
    exit 1
} finally {
    $Host.UI.RawUI.ForegroundColor = $origFG; $Host.UI.RawUI.BackgroundColor = $origBG
    $Host.UI.RawUI.CursorSize = $prevCursor; Write-Host $c_reset -NoNewline
}