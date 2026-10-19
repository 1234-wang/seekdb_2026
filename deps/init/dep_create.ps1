<#
.SYNOPSIS
    Windows dependency initialization script - mirrors dep_create.sh on Linux/macOS.

.DESCRIPTION
    Reads oceanbase.windows.x86_64.deps, downloads tar.gz archives from the
    configured HTTP repo, and extracts them into deps/3rd/.

    After extraction the layout is:
        deps/3rd/vcpkg/x64-windows/   (vcpkg installed packages)
        deps/3rd/openssl/             (OpenSSL)
        deps/3rd/vsag/               (vsag vector search library)
        deps/3rd/tools/cmake/         (CMake)
        deps/3rd/tools/ninja/         (Ninja)
        deps/3rd/tools/llvm18/        (LLVM 18)
        deps/3rd/tools/win_flex_bison/(Flex & Bison)

.EXAMPLE
    powershell -File dep_create.ps1
#>

$ErrorActionPreference = "Stop"
$ScriptDir  = $PSScriptRoot
$TOPDIR     = (Resolve-Path "$ScriptDir\..\..").Path
$DEPS_3RD   = "$TOPDIR\deps\3rd"
$DONE_FILE  = "$DEPS_3RD\DONE"
$DEP_FILE   = "$ScriptDir\oceanbase.windows.x86_64.deps"
$DIRENT_HEADER = "$DEPS_3RD\vcpkg\x64-windows\include\dirent.h"
$DIRENT_PATCH = "$ScriptDir\patches\dirent-1.26-sticky-eof.diff"
$DIRENT_INPUT_SHA256 = "de26601714921e855f45c3318ef01ae19a892db3d77d0c9b769a4f5c5f8478a7"
$DIRENT_OUTPUT_SHA256 = "0a12fc6d3172c2254a27b598447dc313bf3a979fdc3da98a761e380cfc9a4425"

# -- Helpers ----------------------------------------------------------
function Write-Log  { param([string]$msg) Write-Host "[dep_create.ps1] $msg" }
function Write-Err  { param([string]$msg) Write-Host "[dep_create.ps1][ERROR] $msg" -ForegroundColor Red }

function Get-Sha256 {
    param([string]$Path)
    return (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLower()
}

function Test-DirentPatched {
    return (Test-Path $DIRENT_HEADER) -and
        ((Get-Sha256 $DIRENT_HEADER) -eq $DIRENT_OUTPUT_SHA256)
}

# -- Check deps file -------------------------------------------------
if (-not (Test-Path $DEP_FILE)) {
    Write-Err "Dependencies profile not found: $DEP_FILE"
    exit 2
}
Write-Log "Using dependencies profile: $DEP_FILE"

# -- MD5 cache check -------------------------------------------------
$md5 = (Get-FileHash -Path $DEP_FILE -Algorithm MD5).Hash.ToLower()
$MD5_FILE = "$DEPS_3RD\$md5"

if ((Test-Path $MD5_FILE) -and (Test-Path $DONE_FILE) -and (Test-DirentPatched)) {
    Write-Log "Dependencies already initialized (MD5=$md5). Skipping."
    exit 0
}
Write-Log "Initializing dependencies ... (MD5=$md5)"

# -- Parse deps file (INI-like, same format as Linux) ----------------
$targets  = @{}          # target-name -> repo URL
$sections = [ordered]@{} # section-name -> string[] of package filenames

$curSection = ""
$curLines   = [System.Collections.Generic.List[string]]::new()

function Save-Section {
    if ($curSection -eq "") { return }
    if ($curSection -match "^target-(.+)$") {
        $tName = $Matches[1]
        foreach ($l in $curLines) {
            if ($l -match "^repo=(.+)$") {
                $script:targets[$tName] = $Matches[1].Trim()
                Write-Log "  target: $tName  repo: $($Matches[1].Trim())"
            }
        }
    } else {
        $script:sections[$curSection] = @($curLines)
    }
}

foreach ($rawLine in Get-Content $DEP_FILE -Encoding UTF8) {
    $line = $rawLine.Trim()
    if ($line -match "^\[(.+)\]$") {
        Save-Section
        $curSection = $Matches[1]
        $curLines   = [System.Collections.Generic.List[string]]::new()
    }
    elseif ($line -ne "" -and -not $line.StartsWith("#")) {
        $curLines.Add($line)
    }
}
Save-Section

$defaultRepo = if ($targets.ContainsKey("default")) { $targets["default"] } else { "" }
if ($defaultRepo -eq "") {
    Write-Err "No [target-default] repo defined in $DEP_FILE"
    exit 3
}

# -- Prepare deps/3rd ------------------------------------------------
if (Test-Path $DEPS_3RD) {
    Write-Log "Removing existing deps/3rd ..."
    Remove-Item -Recurse -Force $DEPS_3RD
}
New-Item -ItemType Directory -Path "$DEPS_3RD\pkg" -Force | Out-Null

# -- Download & extract ----------------------------------------------
$totalPkgs  = ($sections.Values | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum
$currentPkg = 0

foreach ($sect in $sections.Keys) {
    Write-Log "-- section [$sect] --"
    foreach ($entry in $sections[$sect]) {
        $parts   = $entry -split '\s+'
        $pkgFile = $parts[0]

        # Optional per-package target override: "pkg.tar.gz target=xxx"
        $tName = "default"
        foreach ($p in $parts) {
            if ($p -match "^target=(.+)$") { $tName = $Matches[1] }
        }

        $repo = if ($targets.ContainsKey($tName)) { $targets[$tName] } else { $defaultRepo }
        $url     = "$repo/$pkgFile"
        $pkgPath = "$DEPS_3RD\pkg\$pkgFile"

        $currentPkg++
        Write-Log "[$currentPkg/$totalPkgs] $pkgFile"

        # -- Download ------------------------------------------------
        if (Test-Path $pkgPath) {
            Write-Log "  cached"
        } else {
            Write-Log "  downloading from $url ..."
            $tmpPath = "$pkgPath.tmp"
            try {
                & curl.exe -L -f -s --retry 3 --retry-delay 2 -o $tmpPath $url
                if ($LASTEXITCODE -ne 0) {
                    throw "curl exit code $LASTEXITCODE"
                }
                Move-Item -Force $tmpPath $pkgPath
            }
            catch {
                if (Test-Path $tmpPath) { Remove-Item -Force $tmpPath }
                Write-Err "Failed to download: $url"
                Write-Err "$_"
                exit 4
            }
        }

        # -- Extract -------------------------------------------------
        Write-Log "  extracting ..."
        & tar -xzf $pkgPath -C $DEPS_3RD --strip-components=1
        if ($LASTEXITCODE -ne 0) {
            Write-Err "Failed to extract: $pkgFile"
            exit 5
        }
        Write-Log "  done"
    }
}

# -- Apply repository-owned dependency patches ----------------------
if (-not (Test-Path $DIRENT_HEADER)) {
    Write-Err "dirent header not found after dependency extraction: $DIRENT_HEADER"
    exit 6
}
if (-not (Test-Path $DIRENT_PATCH)) {
    Write-Err "dirent patch not found: $DIRENT_PATCH"
    exit 6
}

$direntHash = Get-Sha256 $DIRENT_HEADER
if ($direntHash -eq $DIRENT_OUTPUT_SHA256) {
    Write-Log "dirent sticky-EOF patch already applied"
} elseif ($direntHash -ne $DIRENT_INPUT_SHA256) {
    Write-Err "Unexpected dirent.h SHA256: $direntHash"
    exit 6
} else {
    Write-Log "Applying dirent sticky-EOF patch ..."
    $patchDirectory = "deps/3rd/vcpkg/x64-windows/include"
    & git -C $TOPDIR apply --unidiff-zero "--directory=$patchDirectory" -- $DIRENT_PATCH
    if ($LASTEXITCODE -ne 0) {
        Write-Err "Failed to apply dirent sticky-EOF patch"
        exit 6
    }
    $patchedHash = Get-Sha256 $DIRENT_HEADER
    if ($patchedHash -ne $DIRENT_OUTPUT_SHA256) {
        Write-Err "Patched dirent.h SHA256 mismatch: $patchedHash"
        exit 6
    }
    Write-Log "dirent sticky-EOF patch applied"
}

# -- Mark done --------------------------------------------------------
New-Item -ItemType File -Path $MD5_FILE  -Force | Out-Null
New-Item -ItemType File -Path $DONE_FILE -Force | Out-Null

Write-Log ""
Write-Log "Dependencies initialized successfully."
Write-Log ""
Write-Log "Layout:"
Write-Log "  deps/3rd/vcpkg/x64-windows/    vcpkg packages"
Write-Log "  deps/3rd/openssl/              OpenSSL"
Write-Log "  deps/3rd/vsag/                 vsag vector search library"
Write-Log "  deps/3rd/tools/cmake/          CMake"
Write-Log "  deps/3rd/tools/ninja/          Ninja"
Write-Log "  deps/3rd/tools/llvm18/         LLVM 18"
Write-Log "  deps/3rd/tools/win_flex_bison/ Flex & Bison"
