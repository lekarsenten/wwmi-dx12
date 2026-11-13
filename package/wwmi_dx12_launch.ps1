# Run through wwmi_dx12_launch.bat, from the game's Client\Binaries\Win64 folder.
# Paths go through .NET: the PowerShell provider reads [ ] in a folder name as a wildcard.
$ErrorActionPreference = "Stop"
$dir    = $PSScriptRoot
$proc   = "Client-Win64-Shipping"
$proxy  = [IO.Path]::Combine($dir, "dxgi.dll")
$ini    = [IO.Path]::Combine($dir, "wwmi_dx12.ini")
$iniSeed = [IO.Path]::Combine($dir, "wwmi_dx12.ini.seed")
$minReShade = [version]"6.8.0"

function Wait-Key {
    Write-Host "Press any key to exit."
    try { [void][Console]::ReadKey($true) } catch { Start-Sleep -Seconds 15 }
}
function Fail([string]$msg) {
    Write-Host ""
    Write-Host $msg -ForegroundColor Red
    Wait-Key
    exit 1
}
trap {
    Write-Host ""
    Write-Host "The launcher stopped on an error: $_" -ForegroundColor Red
    Wait-Key
    exit 2
}

if (-not [IO.File]::Exists($ini) -and [IO.File]::Exists($iniSeed)) {
    try { [IO.File]::Copy($iniSeed, $ini) } catch { Fail "Could not write wwmi_dx12.ini in $dir -- is the game folder read-only?" }
}

$wwmiKey = [regex]'(?m)^[ \t]*wwmi_dir[ \t]*=([^\r\n]*)'
function Get-WwmiSetting {
    if (-not [IO.File]::Exists($ini)) { return "" }
    $m = $wwmiKey.Match([IO.File]::ReadAllText($ini))
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return ""
}
function Save-WwmiSetting([string]$value) {
    $text = if ([IO.File]::Exists($ini)) { [IO.File]::ReadAllText($ini) } else { "[wwmi-dx12]`r`n" }
    if (-not $wwmiKey.IsMatch($text)) { $text = ([regex]'(?m)^\[wwmi-dx12\][^\r\n]*').Replace($text, "`$0`r`nwwmi_dir =", 1) }
    $text = $wwmiKey.Replace($text, "wwmi_dir = $value".Replace('$', '$$'), 1)
    [IO.File]::WriteAllText($ini, $text, [Text.Encoding]::Unicode)
}

function Get-ReShadeAddonPath([string]$file) {
    if (-not [IO.File]::Exists($file)) { return "" }
    $inAddon = $false
    foreach ($line in [IO.File]::ReadAllLines($file, [Text.Encoding]::UTF8)) {
        $t = $line.Trim()
        if ($t.StartsWith("[")) { $inAddon = ($t -ieq "[ADDON]"); continue }
        if ($inAddon -and $t -match '^AddonPath\s*=(.*)$') { return $Matches[1].Trim() }
    }
    return ""
}

function Resolve-Dir([string]$p) {
    if (-not $p) { return "" }
    try {
        if ([IO.Path]::IsPathRooted($p)) { return $p }
        return [IO.Path]::GetFullPath([IO.Path]::Combine($dir, $p))
    } catch { return "" }
}
function Test-WwmiDir([string]$p) {
    if (-not $p) { return $false }
    try {
        return [IO.File]::Exists([IO.Path]::Combine($p, "d3dx.ini")) -and [IO.Directory]::Exists([IO.Path]::Combine($p, "Core\WWMI"))
    } catch { return $false }
}
function Find-WwmiNearGame([string]$from) {
    $found = @()
    $seen = @{}
    $p = $from
    for ($up = 0; $up -lt 3 -and $p; $up++) {
        $p = [IO.Path]::GetDirectoryName($p)
        if (-not $p -or $seen[$p]) { break }
        $seen[$p] = $true
        Write-Host "  looking in $p ..."
        $dirs = @(Get-ChildItem -LiteralPath $p -Directory -Recurse -Depth 2 -ErrorAction SilentlyContinue)
        foreach ($d in $dirs) {
            $cand = $d.FullName
            if ((Test-WwmiDir $cand) -and ($found -notcontains $cand)) { $found += $cand }
            $xxmi = [IO.Path]::Combine($cand, "WWMI")
            if ([IO.File]::Exists([IO.Path]::Combine($cand, "XXMI Launcher Config.json")) -and (Test-WwmiDir $xxmi) -and ($found -notcontains $xxmi)) { $found += $xxmi }
        }
        if ($found.Count -gt 0) { break }
    }
    return ,$found
}
function Select-Found($found) {
    if ($found.Count -eq 1) { return $found[0] }
    for ($i = 0; $i -lt $found.Count; $i++) { Write-Host "  [$($i + 1)] $($found[$i])" }
    $pick = Read-Host "Which one (1-$($found.Count))"
    $n = 0
    if (-not [int]::TryParse($pick, [ref]$n) -or $n -lt 1 -or $n -gt $found.Count) { Fail "No folder chosen." }
    return $found[$n - 1]
}
function Test-ReShade([string]$dll) {
    return [Diagnostics.FileVersionInfo]::GetVersionInfo($dll).ProductName -match "ReShade"
}
function Get-WwmiVersion([string]$p) {
    try {
        $ini = [IO.Path]::Combine($p, "Core\WWMI\WuWa-Model-Importer.ini")
        if (-not [IO.File]::Exists($ini)) { return "not found" }
        $m = [regex]::Match([IO.File]::ReadAllText($ini), '(?m)^[ \t]*global[ \t]+(?:persist[ \t]+)?\$wwmi_version[ \t]*=[ \t]*([0-9.]+)')
        if ($m.Success) { return $m.Groups[1].Value }
        return "unknown"
    } catch { return "unknown" }
}
function Test-GameRunning { return [bool](Get-Process $proc -ErrorAction SilentlyContinue) }

function Get-AddonFile {
    $files = @([IO.Path]::Combine($dir, "wwmi_dx12.addon64"))
    $addonPathDir = Resolve-Dir (Get-ReShadeAddonPath ([IO.Path]::Combine($dir, "ReShade.ini")))
    if ($addonPathDir) { $files += [IO.Path]::Combine($addonPathDir, "wwmi_dx12.addon64") }
    return $files | Where-Object { [IO.File]::Exists($_) } | Sort-Object { [IO.File]::GetLastWriteTimeUtc($_) } | Select-Object -Last 1
}
function Get-AddonVersion {
    try {
        $newest = Get-AddonFile
        if (-not $newest) { return "not found" }
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($newest)
        if (-not $info.ProductVersion) { return "unknown" }
        if ($info.Comments) { return "$($info.ProductVersion) (build $($info.Comments))" }
        return $info.ProductVersion
    } catch { return "unknown" }
}
function Get-AddonRelease {
    try {
        $newest = Get-AddonFile
        $v = $null
        if ($newest -and [version]::TryParse([Diagnostics.FileVersionInfo]::GetVersionInfo($newest).ProductVersion, [ref]$v)) { return $v }
    } catch {}
    return $null
}

# Newer releases from GitHub, newest first, each with its release commit's message as the notes.
function Start-UpdateCheck([version]$have) {
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $shell = [PowerShell]::Create()
        [void]$shell.AddScript({
            param([version]$have)
            $api = "https://api.github.com/repos/lekarsenten/wwmi-dx12"
            $headers = @{ "User-Agent" = "wwmi-dx12-launcher"; "Accept" = "application/vnd.github+json" }
            $releases = Invoke-RestMethod "$api/releases?per_page=100" -Headers $headers -TimeoutSec 15
            $newer = @()
            foreach ($r in $releases) {
                $v = $null
                if ($r.draft -or $r.prerelease -or -not [version]::TryParse(($r.tag_name -replace '^v', ''), [ref]$v) -or $v -le $have) { continue }
                $notes = ""
                try { $notes = (Invoke-RestMethod "$api/commits/$($r.tag_name)" -Headers $headers -TimeoutSec 15).commit.message } catch {}
                $newer += [pscustomobject]@{ Version = $v; Notes = $notes }
            }
            $newer | Sort-Object Version -Descending
        }).AddArgument($have)
        return @{ Shell = $shell; Run = $shell.BeginInvoke() }
    } catch { return $null }
}
function Receive-UpdateCheck($check) {
    try { return @($check.Shell.EndInvoke($check.Run)) } catch { return @() }
}

Write-Host "WWMI-DX12 launcher" -ForegroundColor Cyan
if (-not [IO.File]::Exists([IO.Path]::Combine($dir, "$proc.exe"))) {
    Fail "This launcher must be in the game's Client\Binaries\Win64 folder (next to $proc.exe)."
}
if (Test-GameRunning) { Fail "The game is already running." }
$mutex = New-Object Threading.Mutex($false, "wwmi_dx12_launch")
$owned = $false
try { $owned = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owned = $true }
if (-not $owned) { Fail "The launcher is already running (look for its window)." }

$root = [IO.Path]::GetFullPath([IO.Path]::Combine($dir, "..\..\.."))
$exe = [IO.Path]::Combine($root, "Wuthering Waves.exe")
if (-not [IO.File]::Exists($exe)) { Fail "Wuthering Waves.exe not found in $root." }

$addonDir = $dir
$addonPath = Get-ReShadeAddonPath ([IO.Path]::Combine($dir, "ReShade.ini"))
if ($addonPath) { $addonDir = Resolve-Dir $addonPath }
$addonHere = [IO.Path]::Combine($dir, "wwmi_dx12.addon64")
if ($addonDir -and [IO.File]::Exists($addonHere)) {
    $addonThere = [IO.Path]::GetFullPath([IO.Path]::Combine($addonDir, "wwmi_dx12.addon64"))
    if (($addonThere -ine [IO.Path]::GetFullPath($addonHere)) -and
        (-not [IO.File]::Exists($addonThere) -or [IO.File]::GetLastWriteTimeUtc($addonHere) -gt [IO.File]::GetLastWriteTimeUtc($addonThere))) {
        [void][IO.Directory]::CreateDirectory($addonDir)
        [IO.File]::Copy($addonHere, $addonThere, $true)
        Write-Host "wwmi_dx12.addon64 copied to $addonDir (ReShade.ini's AddonPath)."
    }
}
if (-not $addonDir -or -not [IO.File]::Exists([IO.Path]::Combine($addonDir, "wwmi_dx12.addon64"))) {
    Fail "wwmi_dx12.addon64 is not in '$addonDir' (where ReShade loads add-ons from; ReShade.ini says AddonPath='$addonPath'). See README: Install."
}

$wwmi = Get-WwmiSetting
$resolved = Resolve-Dir $wwmi
$save = $false
if (-not (Test-WwmiDir $resolved)) {
    $save = $true
    if ($wwmi) { Write-Host "wwmi_dir in wwmi_dx12.ini is not a WWMI folder: $wwmi" -ForegroundColor Yellow }
    $xxmi = if ($env:APPDATA) { [IO.Path]::Combine($env:APPDATA, "XXMI Launcher\WWMI") } else { "" }
    $near = @()
    if (-not (Test-WwmiDir $xxmi)) {
        Write-Host "Looking for WWMI near the game..."
        $near = Find-WwmiNearGame ([IO.Path]::GetFullPath([IO.Path]::Combine($dir, "..\..\..")))
    }
    if (Test-WwmiDir $xxmi) {
        $resolved = $xxmi
        Write-Host "Found WWMI in XXMI Launcher's folder: $xxmi"
    } elseif ($near.Count -gt 0) {
        $resolved = Select-Found $near
        Write-Host "Found WWMI near the game: $resolved"
    } else {
        $answer = (Read-Host "Path to your WWMI folder (the one with d3dx.ini), or Enter to search all drives").Trim('" ')
        if ($answer) {
            if (-not (Test-WwmiDir $answer)) { Fail "Not a WWMI folder (needs d3dx.ini and Core\WWMI): $answer" }
            $resolved = $answer
        } else {
            Write-Host "Searching fixed drives for WWMI (this can take a few minutes)..."
            $found = @()
            foreach ($drv in [IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady }) {
                Get-ChildItem -LiteralPath $drv.RootDirectory.FullName -Filter "d3dx.ini" -Recurse -File -ErrorAction SilentlyContinue |
                    ForEach-Object {
                        $cand = $_.Directory.FullName
                        if ((Test-WwmiDir $cand) -and ($found -notcontains $cand)) { $found += $cand; Write-Host "  found: $cand" }
                    }
            }
            if ($found.Count -eq 0) { Fail "No WWMI folder found. Install WWMI (XXMI Launcher) first, or enter its path." }
            $resolved = Select-Found $found
        }
    }
}
if ($resolved.Length -ge 248) { Fail "The WWMI folder's path is too long for the add-on ($($resolved.Length) characters; under 248 works): $resolved" }
if ($save) {
    try { Save-WwmiSetting $resolved } catch { Fail "Could not write wwmi_dx12.ini in $dir -- is the game folder read-only?" }
    Write-Host "WWMI folder saved to wwmi_dx12.ini: $resolved"
}

if (-not [IO.File]::Exists($proxy)) {
    Fail "ReShade not found. Install ReShade with full add-on support into this folder. See README."
}
if (-not (Test-ReShade $proxy)) {
    Fail "dxgi.dll in this folder is not ReShade. Remove or rename it, then install ReShade (with full add-on support) here."
}
$reshadeInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($proxy)
$reshadeVersion = New-Object Version($reshadeInfo.FileMajorPart, $reshadeInfo.FileMinorPart, $reshadeInfo.FileBuildPart)
if ($reshadeVersion -lt $minReShade) {
    Fail "ReShade $reshadeVersion (dxgi.dll) is too old: the add-on needs ReShade $minReShade or newer. Install the current ReShade, with full add-on support, here."
}

# Read-only while the game starts: it rewrites Engine.ini and drops settings it does not keep.
$engineIni = [IO.Path]::Combine($root, "Client\Saved\Config\WindowsNoEditor\Engine.ini")
$engineCvars = [ordered]@{
    "r.TextureStreaming"                = "1"
    "r.Streaming.FullyLoadUsedTextures" = "1"
    "r.Streaming.Boost"                 = "30"
    "r.Streaming.MinBoost"              = "30"
    "r.Streaming.HLODStrategy"          = "2"
    "r.MipMapLODBias"                   = "-1"
}
# As XXMI Launcher writes it for WWMI: characters keep the model mods replace out to the farthest camera.
$userEngineIni = [IO.Path]::Combine($root, "Client\Config\UserEngine.ini")
$userEngineCvars = [ordered]@{
    "r.Kuro.SkeletalMesh.DistanceLODBaseFOV" = "165"
}
function Set-IniValues([string]$path, [string]$section, $values) {
    $lines = New-Object Collections.Generic.List[string]
    if ([IO.File]::Exists($path)) {
        [IO.File]::SetAttributes($path, [IO.File]::GetAttributes($path) -band -bnot [IO.FileAttributes]::ReadOnly)
        foreach ($l in [IO.File]::ReadAllLines($path)) { $lines.Add($l) }
    } else {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    }
    $header = "[$section]"
    $out = New-Object Collections.Generic.List[string]
    $seen = @{}
    $inSection = $false
    $hasSection = $false
    foreach ($l in $lines) {
        $t = $l.Trim()
        if ($t.StartsWith("[") -and $t.EndsWith("]")) {
            if ($inSection) { foreach ($k in $values.Keys) { if (-not $seen[$k]) { $out.Add("$k=$($values[$k])"); $seen[$k] = $true } } }
            $inSection = ($t -ieq $header)
            if ($inSection) { $hasSection = $true }
        } elseif ($inSection -and $t.Contains("=")) {
            $k = $t.Split("=", 2)[0].Trim()
            if ($values.Contains($k)) { $l = "$k=$($values[$k])"; $seen[$k] = $true }
        }
        $out.Add($l)
    }
    if ($inSection) { foreach ($k in $values.Keys) { if (-not $seen[$k]) { $out.Add("$k=$($values[$k])") } } }
    if (-not $hasSection) {
        $out.Add("")
        $out.Add($header)
        foreach ($k in $values.Keys) { $out.Add("$k=$($values[$k])") }
    }
    [IO.File]::WriteAllLines($path, $out)
}
function Set-EngineCvars {
    Set-IniValues $engineIni "SystemSettings" $engineCvars
    [IO.File]::SetAttributes($engineIni, [IO.File]::GetAttributes($engineIni) -bor [IO.FileAttributes]::ReadOnly)
}
function Clear-EngineReadOnly {
    if ([IO.File]::Exists($engineIni)) {
        [IO.File]::SetAttributes($engineIni, [IO.File]::GetAttributes($engineIni) -band -bnot [IO.FileAttributes]::ReadOnly)
    }
}

$tiers = @("uhd", "hd", "sd")
$installed = @($tiers | Where-Object { [IO.Directory]::Exists([IO.Path]::Combine($root, "Client", "Content", $_.ToUpper())) })
$tier = if ($installed.Count) { $installed[0] } else { "hd" }

function Write-Graphics {
    Write-Host -NoNewline "  Graphics       "
    foreach ($t in $tiers) {
        $name = $t.ToUpper()
        if ($t -eq $tier) { Write-Host -NoNewline "[$name]" -ForegroundColor Cyan }
        elseif ($installed -contains $t) { Write-Host -NoNewline " $name " }
        else { Write-Host -NoNewline " $name " -ForegroundColor DarkGray }
        Write-Host -NoNewline "  "
    }
    $note = if ($installed -contains $tier) { "(installed)" } else { "(not found in this game folder)" }
    Write-Host $note.PadRight(34)
}
$have = Get-AddonRelease
$updateCheck = if ($have) { Start-UpdateCheck $have } else { $null }
$update = @()
$updateRow = -1
$updateBright = $true
$updateFlashAt = Get-Date
$updateDots = 0
function Write-UpdateLine {
    if (-not $update.Count) {
        $wait = if ($updateCheck) { "  Checking for updates" + ("." * $updateDots) } else { "" }
        Write-Host -NoNewline $wait.PadRight(30)
        return
    }
    $text = " Please update: wwmi-dx12 $($update[0].Version) is out (this add-on is $have). "
    Write-Host -NoNewline "  "
    if ($updateBright) { Write-Host -NoNewline $text -ForegroundColor Black -BackgroundColor Yellow }
    else { Write-Host -NoNewline $text -ForegroundColor Yellow }
}
function Write-UpdateLineInPlace {
    try {
        $back = [Console]::CursorTop
        [Console]::SetCursorPosition(0, $updateRow)
        Write-UpdateLine
        [Console]::SetCursorPosition(0, $back)
    } catch { $script:updateRow = -1 }
}
function Get-Wrapped([string]$text, [string]$indent) {
    $width = 100
    try { $width = [Console]::WindowWidth - 1 } catch {}
    $hang = if ($text.StartsWith("- ")) { $indent + "  " } else { $indent }
    $line = $indent
    $fresh = $true
    foreach ($word in $text.Split(' ')) {
        if (-not $fresh -and $line.Length + 1 + $word.Length -gt $width) {
            $line
            $line = $hang
            $fresh = $true
        }
        $line += $(if ($fresh) { "" } else { " " }) + $word
        $fresh = $false
    }
    $line
}
# The notes under the update line, cut to the rows left in the window: more would scroll it, and the lines
# redrawn in place would move.
function Show-Update {
    $rows = @()
    foreach ($u in $update) {
        $lines = @(($u.Notes -replace "`r", "").Split("`n") | Where-Object { $_.Trim() })
        if ($lines.Count -and $lines[0] -match '^wwmi-dx12 ') { $lines = @($lines | Select-Object -Skip 1) }
        $rows += @{ Text = "" }
        $rows += @{ Text = "  What's new in $($u.Version)"; Color = "Cyan" }
        foreach ($l in $lines) { foreach ($w in @(Get-Wrapped $l.Trim() "    ")) { $rows += @{ Text = $w } } }
    }
    $room = 0
    try { $room = [Console]::WindowTop + [Console]::WindowHeight - 1 - [Console]::CursorTop } catch {}
    if ($rows.Count -gt $room) {
        $rows = @($rows | Select-Object -First ([Math]::Max($room - 1, 0)))
        if ($room -ge 1) { $rows += @{ Text = "    ... more: https://github.com/lekarsenten/wwmi-dx12/releases"; Color = "DarkGray" } }
    }
    foreach ($r in $rows) {
        if ($r.Color) { Write-Host $r.Text -ForegroundColor $r.Color } else { Write-Host $r.Text }
    }
}

Write-Host ""
Write-Host "  Add-on         $(Get-AddonVersion)"
Write-Host "  Game folder    $root"
Write-Host "  WWMI folder    $resolved"
Write-Host "  WWMI version   $(Get-WwmiVersion $resolved)"
Write-Host "  ReShade        $reshadeVersion"
$graphicsRow = -1
try { $graphicsRow = [Console]::CursorTop } catch {}
Write-Graphics
Write-Host ""
Write-Host "  Left / Right   choose the graphics files"
Write-Host "  Esc            quit"
Write-Host "  any other key  start the game"
if ($updateCheck) {
    Write-Host ""
    try { $updateRow = [Console]::CursorTop } catch {}
    Write-UpdateLine
    Write-Host ""
}
while ($true) {
    try { $ready = [Console]::KeyAvailable } catch { break }
    if (-not $ready) {
        if ($updateCheck -and $updateCheck.Run.IsCompleted) {
            $update = @(Receive-UpdateCheck $updateCheck)
            $updateCheck = $null
            if ($updateRow -ge 0) { Write-UpdateLineInPlace }
            if (-not $update.Count) { $updateRow = -1 }
            elseif ($updateRow -ge 0) { Show-Update }
        }
        if ($updateRow -ge 0 -and (Get-Date) -ge $updateFlashAt) {
            if ($update.Count) {
                $updateBright = -not $updateBright
                $updateFlashAt = (Get-Date).AddMilliseconds(1500)
            } else {
                $updateDots = ($updateDots + 1) % 4
                $updateFlashAt = (Get-Date).AddMilliseconds(400)
            }
            Write-UpdateLineInPlace
        }
        Start-Sleep -Milliseconds 50
        continue
    }
    try { $key = [Console]::ReadKey($true) } catch { break }
    if ($key.Key -eq [ConsoleKey]::Escape) { exit 0 }
    $step = 0
    if (@([ConsoleKey]::LeftArrow, [ConsoleKey]::UpArrow) -contains $key.Key) { $step = -1 }
    elseif (@([ConsoleKey]::RightArrow, [ConsoleKey]::DownArrow, [ConsoleKey]::Tab) -contains $key.Key) { $step = 1 }
    if ($step -eq 0) { break }
    $tier = $tiers[([array]::IndexOf($tiers, $tier) + $step + $tiers.Count) % $tiers.Count]
    try {
        $back = [Console]::CursorTop
        [Console]::SetCursorPosition(0, $graphicsRow)
        Write-Graphics
        [Console]::SetCursorPosition(0, $back)
    } catch { Write-Graphics }
}
if ($updateCheck) { try { [void]$updateCheck.Shell.BeginStop($null, $null) } catch {} }
Write-Host ""

function Hide-LauncherWindow {
    try {
        Add-Type -Namespace WwmiDx12 -Name Console -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
'@
        $window = [WwmiDx12.Console]::GetConsoleWindow()
        if ($window -ne [IntPtr]::Zero) { [void][WwmiDx12.Console]::ShowWindow($window, 6) }
    } catch {}
}

$problem = ""
try {
    Set-EngineCvars
    Set-IniValues $userEngineIni "ConsoleVariables" $userEngineCvars
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $exe
    $start.Arguments = "-krqlv=$tier -dx12"
    $start.WorkingDirectory = $root
    [void][Diagnostics.Process]::Start($start)
    $t0 = Get-Date
    while (-not (Test-GameRunning)) {
        if (((Get-Date) - $t0).TotalSeconds -gt 600) { $problem = "The game did not start within 10 minutes."; break }
        Start-Sleep -Seconds 1
    }
    if (-not $problem) {
        Write-Host "Game starting. This window minimizes and closes by itself in a minute, once the game has read its settings."
        Hide-LauncherWindow
        Start-Sleep -Seconds 60
    }
}
finally {
    Clear-EngineReadOnly
}
if ($problem) { Fail $problem }
