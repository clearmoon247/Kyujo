# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
<#
  Kyujo - Windows setup.

  What it does, in order (each step prints what it is doing; everything is also written to work\logs\setup.log):
    1. Checks the computer (64-bit Windows 10 or 11) and the folder this kit sits in.
    2. Asks for YOUR ROM (a file dialog, or drop the file on Setup.bat) and checks that it is the supported game (USA) v1.0.
    3. Gets the compiler: uses downloads\llvm-mingw-*.zip when it is there and its SHA-256 matches, otherwise downloads it
       (online edition) and checks the same SHA-256. It is unpacked into work\tc.
    4. Gets the game's source code (the decompilation, one pinned commit): from downloads\ when present, otherwise from GitHub
       (online edition). Every file is checked against app\port\decomp-manifest.txt; anything that differs stops the setup.
    5. Builds Game\kyujo.exe on THIS computer (a few minutes) and runs the program checks on it.
    6. Writes Game\kyujo.cfg (your ROM), makes a shortcut and starts the game.

  Nothing taken from your ROM is stored: the ROM is only read at the end to be checked, and the game reads it again every time it starts.
  The setup writes only inside this kit's folder (and the shortcut next to it). It never installs anything and never needs administrator rights.

  Parameters (all optional):
    -Rom <file>     the ROM; otherwise a file dialog opens
    -Offline        never use the network (fails if something is missing from downloads\)
    -NoLaunch       do not start the game at the end
    -Rebuild        throw away the previous build and make it again from scratch
    -Python <file>  developers: use this Python instead of the one in the compiler package
    -AllowLongPath  do not stop when the kit's folder path is long
  The ASCII-only text of this file is on purpose: Windows PowerShell 5.1 reads a file without a byte-order mark in the ANSI code page.
#>
[CmdletBinding()]
param(
    [string]$Rom = '',
    [switch]$Offline,
    [switch]$NoLaunch,
    [switch]$Rebuild,
    [string]$Python = '',
    [switch]$AllowLongPath
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'      # the progress bar of Invoke-WebRequest makes downloads many times slower in Windows PowerShell 5.1

# ---------------------------------------------------------------------------------------------------------------- places
$Kit      = Split-Path -Parent $PSScriptRoot
$Downloads = Join-Path $Kit 'downloads'
$Work     = Join-Path $Kit 'work'
$Tc       = Join-Path $Work 'tc'              # the unpacked compiler package (short name: Windows paths are limited to 260 characters)
$Build    = Join-Path $Work 'build'
$DecompDir = Join-Path $Work 'decomp'         # the game's source, unpacked by PowerShell (the compiler package's Python has no zlib, so it cannot read a zip)
$Logs     = Join-Path $Work 'logs'
$Game     = Join-Path $Kit 'Game'
$App      = Join-Path $Kit 'app'
$SetupLog = Join-Path $Logs 'setup.log'
$Components = Get-Content -Raw -Path (Join-Path $PSScriptRoot 'components.json') | ConvertFrom-Json
$Sep = [IO.Path]::DirectorySeparatorChar

# ---------------------------------------------------------------------------------------------------------------- output
function Say([string]$Text, [string]$Color = '') {
    if ($Color) { Write-Host $Text -ForegroundColor $Color } else { Write-Host $Text }
    try { Add-Content -Path $SetupLog -Value ("{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $Text) -Encoding UTF8 } catch { }
}
function Step([string]$Text) { Say ''; Say ("== " + $Text) 'Cyan' }
function Fail([string]$Text) { throw $Text }

function Format-Size([double]$Bytes) {
    if ($Bytes -ge 1MB) { return ('{0:N0} MB' -f ($Bytes / 1MB)) }
    return ('{0:N0} KB' -f ($Bytes / 1KB))
}

# ---------------------------------------------------------------------------------------------------------------- files
function Get-Sha256([string]$Path) { return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant() }

# Downloads $Url to $Dest (through $Dest.part, so a stopped download never looks finished). Shows the percentage while it waits.
function Save-Download([string]$Url, [string]$Dest, [double]$ExpectedSize) {
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
    $part = $Dest + '.part'
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        if (Test-Path $part) { Remove-Item -Force $part }
        $client = New-Object System.Net.WebClient
        try {
            $task = $client.DownloadFileTaskAsync($Url, $part)
            $last = -1
            while (-not $task.IsCompleted) {
                Start-Sleep -Milliseconds 700
                if ($ExpectedSize -gt 0 -and (Test-Path $part)) {
                    $pct = [int](100 * (Get-Item $part).Length / $ExpectedSize)
                    if ($pct -ne $last) { Write-Host -NoNewline ("`r   downloading... {0}%   " -f [Math]::Min($pct, 100)); $last = $pct }
                }
            }
            Write-Host ''
            if ($task.IsFaulted) { throw $task.Exception.GetBaseException() }
            Move-Item -Force $part $Dest
            return
        } catch {
            Write-Host ''
            Say ("   download attempt {0} failed: {1}" -f $attempt, $_.Exception.Message) 'Yellow'
            if ($attempt -eq 3) { Fail ("Could not download $Url`nCheck the internet connection, or download the file in a browser and put it in the folder:`n   $Downloads") }
            Start-Sleep -Seconds (2 * $attempt)
        } finally { $client.Dispose() }
    }
}

# Unpacks $Zip into $Dest, removing the folder $Top from the names, leaving out the entries that match a wildcard of $Skip and moving
# the entries under a prefix of $Map (objects with from, to and an optional skip list) to another place. Refuses any entry that would land outside $Dest.
function Expand-Filtered([string]$Zip, [string]$Dest, [string]$Top, [string[]]$Skip, $Map) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $root = [IO.Path]::GetFullPath($Dest).TrimEnd($Sep) + $Sep
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    $count = 0; $made = @{}
    try {
        foreach ($e in $archive.Entries) {
            $name = $e.FullName
            if (-not $name.StartsWith($Top)) { continue }
            $rel = $name.Substring($Top.Length)
            if ($rel.Length -eq 0) { continue }
            $skipped = $false
            foreach ($pattern in $Skip) { if ($rel -like $pattern) { $skipped = $true; break } }
            if ($skipped) { continue }
            foreach ($m in @($Map)) {
                if ($rel.StartsWith($m.from)) {
                    foreach ($pattern in @($m.skip)) { if ($rel -like $pattern) { $skipped = $true; break } }
                    $rel = $m.to + $rel.Substring($m.from.Length)
                    break
                }
            }
            if ($skipped -or $rel.Length -eq 0) { continue }
            $target = [IO.Path]::GetFullPath((Join-Path $Dest ($rel.Replace('/', $Sep))))
            if (-not $target.StartsWith($root)) { Fail "The zip contains an unsafe path: $name" }
            if ($rel.EndsWith('/')) { [void][IO.Directory]::CreateDirectory($target); continue }
            $dir = [IO.Path]::GetDirectoryName($target)
            if (-not $made.ContainsKey($dir)) { [void][IO.Directory]::CreateDirectory($dir); $made[$dir] = 1 }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
            $count++
            if ($count % 1500 -eq 0) { Write-Host -NoNewline ("`r   unpacked {0} files...   " -f $count) }
        }
    } finally { $archive.Dispose() }
    Write-Host ("`r   unpacked {0} files.            " -f $count)
}

# Runs a program and returns @{ Code; Output } (Output = all its text, one string). In Windows PowerShell 5.1 any text a program writes to its error
# channel becomes a terminating error when $ErrorActionPreference is 'Stop', so it is switched off for the call.
function Invoke-Native([string]$Program, [string[]]$Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $text = & $Program @Arguments 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    return [pscustomobject]@{ Code = $code; Output = (@($text) -join "`n") }
}

# Runs a program, writes its output to $LogFile and shows new lines on the screen while it runs. Returns the exit code.
function Invoke-Logged([string]$Program, [string[]]$Arguments, [string]$LogFile, [string]$Indent = '   ') {
    $quoted = ($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $err = $LogFile + '.err'
    foreach ($f in @($LogFile, $err)) { if (Test-Path $f) { Remove-Item -Force $f } }
    $p = Start-Process -FilePath $Program -ArgumentList $quoted -NoNewWindow -PassThru -RedirectStandardOutput $LogFile -RedirectStandardError $err
    $null = $p.Handle          # without this, Windows PowerShell 5.1 may report $null as the exit code of a process it did not wait for
    $shown = 0
    while (-not $p.HasExited) {
        Start-Sleep -Milliseconds 1500
        $shown = Show-NewLines $LogFile $shown $Indent
    }
    $p.WaitForExit()
    [void](Show-NewLines $LogFile $shown $Indent)
    if ((Test-Path $err) -and (Get-Item $err).Length -gt 0) { Add-Content -Path $LogFile -Value "`n--- messages on the error channel ---" ; Get-Content $err -Encoding UTF8 | Add-Content -Path $LogFile }
    return $p.ExitCode
}
function Show-NewLines([string]$File, [int]$Already, [string]$Indent) {
    if (-not (Test-Path $File)) { return $Already }
    $lines = @(Get-Content -Path $File -Encoding UTF8 -ErrorAction SilentlyContinue)
    for ($i = $Already; $i -lt $lines.Count; $i++) { Write-Host ($Indent + $lines[$i]) }
    return $lines.Count
}

# ---------------------------------------------------------------------------------------------------------------- the steps
function Test-Computer {
    Step '1. Checking this computer'
    $onWindows = $true
    if (Test-Path variable:IsWindows) { $onWindows = [bool]$IsWindows }
    if ($onWindows) {
        if (-not [Environment]::Is64BitOperatingSystem) { Fail 'This setup needs 64-bit Windows 10 or 11 (the game itself is a 32-bit program, which 64-bit Windows runs).' }
        if ([Environment]::OSVersion.Version.Major -lt 10) { Fail 'This setup needs Windows 10 or 11.' }
    }
    Say ("   kit folder: " + $Kit)
    $limit = 100
    if ($Kit.Length -gt $limit -and -not $AllowLongPath) {
        Fail ("The folder this kit is in has a long path ({0} characters):`n   {1}`nThe build creates files with long names inside it, and Windows stops working above 260 characters in total.`nMove the whole folder to a short place such as C:\Kyujo and run Setup.bat again (or run with -AllowLongPath if long paths are switched on)." -f $Kit.Length, $Kit)
    }
    if ($Kit -match '[^\x20-\x7E]') { Say '   note: the folder name has letters outside plain English; this was not tested. If something fails, move the kit to e.g. C:\Kyujo.' 'Yellow' }
    foreach ($d in @($Downloads, $Work, $Logs, $Game)) { [void][IO.Directory]::CreateDirectory($d) }
}

function Select-Rom {
    Step '2. Your game ROM'
    $path = $Rom
    if (-not $path) {
        Say '   A window will ask for your ROM file (.z64, .v64 or .n64). The game needs your own copy; it is not part of this kit.'
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dialog = New-Object System.Windows.Forms.OpenFileDialog
            $dialog.Title = 'Select your Pokemon Stadium (USA) ROM'
            $dialog.Filter = 'Nintendo 64 ROM (*.z64;*.v64;*.n64)|*.z64;*.v64;*.n64|All files (*.*)|*.*'
            if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $path = $dialog.FileName }
        } catch { $path = '' }
        if (-not $path) { $path = Read-Host '   Type or paste the full path of the ROM file' }
        $path = $path.Trim('"', ' ')
    }
    if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "No ROM file was chosen (or the file does not exist): $path" }
    $path = (Resolve-Path -LiteralPath $path).ProviderPath
    # A quick check that costs nothing, before anything is downloaded: size and the first four bytes. The full check (MD5) follows once Python is there.
    $info = Get-Item -LiteralPath $path
    if ($info.Length -ne 33554432) { Fail ("That file is {0:N0} bytes. The game ROM is exactly 33,554,432 bytes (32 MiB): {1}" -f $info.Length, $path) }
    $head = New-Object byte[] 4
    $stream = [IO.File]::OpenRead($path)
    try { [void]$stream.Read($head, 0, 4) } finally { $stream.Dispose() }
    $hex = ($head | ForEach-Object { $_.ToString('x2') }) -join ''
    if (@('80371240', '37804012', '40123780') -notcontains $hex) { Fail "That file does not start like a Nintendo 64 ROM (first bytes $hex): $path" }
    Say ("   ROM file: " + $path)
    return $path
}

function Get-Toolchain {
    Step '3. Compiler and Python'
    $c = $Components.toolchain
    Say ("   " + $c.title)
    $marker = Join-Path $Tc '.complete'
    $complete = (Test-Path $marker) -and ((Get-Content -Raw $marker).Trim() -eq $c.sha256)
    if ($complete) { foreach ($n in $c.needs) { if (-not (Test-Path (Join-Path $Tc ($n.Replace('/', $Sep))))) { $complete = $false } } }
    if ($complete) { Say '   already unpacked and complete.'; return }
    $zip = Join-Path $Downloads $c.file
    $ok = $false
    if (Test-Path $zip) {
        Say '   found in downloads\ - checking its SHA-256...'
        if ((Get-Sha256 $zip) -eq $c.sha256) { $ok = $true } else { Say '   that file is not the expected one (SHA-256 differs); it will not be used.' 'Yellow'; Fail "downloads\$($c.file) does not match the expected SHA-256 $($c.sha256). Delete it and run Setup again." }
    }
    if (-not $ok) {
        if ($Offline) { Fail "Offline mode, and downloads\$($c.file) is missing. Put the file there (from $($c.url))." }
        Say ("   downloading {0} from github.com ({1})..." -f $c.file, (Format-Size $c.size))
        Save-Download $c.url $zip $c.size
        if ((Get-Sha256 $zip) -ne $c.sha256) { Remove-Item -Force $zip; Fail "The downloaded compiler package does not match the expected SHA-256 ($($c.sha256)). It was deleted; run Setup again, and if it repeats, report it." }
    }
    Say '   SHA-256 ok. Unpacking (only the parts needed for 32-bit Windows)...'
    if (Test-Path $Tc) { Remove-Item -Recurse -Force $Tc }
    Expand-Filtered $zip $Tc $c.top @($c.skip) @($c.map)
    foreach ($n in $c.needs) { if (-not (Test-Path (Join-Path $Tc ($n.Replace('/', $Sep))))) { Fail "After unpacking, $n is missing from work\tc." } }
    Set-Content -Path $marker -Value $c.sha256 -Encoding ASCII
}

function Find-Python {
    if ($Python) { if (-not (Test-Path $Python)) { Fail "-Python $Python does not exist" }; return (Resolve-Path $Python).ProviderPath }
    $own = Join-Path $Tc 'python\bin\python.exe'
    if (-not (Test-Path $own)) { Fail 'The compiler package has no Python (work\tc\python\bin\python.exe).' }
    return $own
}

function Test-Python([string]$Py) {
    # The Python in the llvm-mingw package is a small build: it has NO zlib (and no bz2, lzma, ssl), so it cannot read compressed zip files.
    # The setup therefore unpacks every zip itself (PowerShell) and this check asks only for what the build really imports.
    $test = 'import sys, hashlib, concurrent.futures, subprocess, json, struct, difflib, tempfile, argparse, re, glob, pathlib, shutil, os; hashlib.md5().hexdigest(); hashlib.sha256().hexdigest(); print(sys.version.split()[0])'
    $r = Invoke-Native $Py @('-X', 'utf8', '-I', '-c', $test)
    $out = $r.Output
    if ($r.Code -ne 0) {
        Fail ("The Python that came with the compiler could not start its basic modules:`n   $out`nInstall Python 3.8 or newer from python.org (tick 'Add python.exe to PATH'), then run:`n   setup\Setup.ps1 -Python <full path of python.exe>")
    }
    Say ("   Python " + $out + " works.")
}

function Confirm-Rom([string]$Py, [string]$RomPath) {
    $tool = Join-Path $App 'tools\rom_tool.py'
    $r = Invoke-Native $Py @('-X', 'utf8', '-I', $tool, 'check', $RomPath)
    Say ("   " + $r.Output)
    if ($r.Code -ne 0) {
        Fail "That ROM is not the supported one. This port needs the North American release (Pokemon Stadium), version 1.0 (md5 ed1378bc12115f71209a77844965ba50 in .z64 byte order)."
    }
    # a .v64 or .n64 file is converted once into the game folder (the game reads the big-endian .z64 form)
    $head = New-Object byte[] 4
    $stream = [IO.File]::OpenRead($RomPath)
    try { [void]$stream.Read($head, 0, 4) } finally { $stream.Dispose() }
    if ((($head | ForEach-Object { $_.ToString('x2') }) -join '') -eq '80371240') { return $RomPath }
    $z64 = Join-Path $Game 'kyujo-rom.z64'
    Say '   converting the ROM to the .z64 byte order (written next to the game, your original is not touched)...'
    $r = Invoke-Native $Py @('-X', 'utf8', '-I', $tool, 'convert', $RomPath, $z64)
    if ($r.Code -ne 0) { Fail ("Could not convert the ROM: " + $r.Output) }
    return $z64
}

# Unpacks the decomp zip into work\decomp and checks every file against the manifest. Returns $true when they all match.
# Only the files the manifest lists are unpacked (the build reads nothing else), the zip's top folder dropped. PowerShell does the unpacking because
# the Python in the compiler package has no zlib and cannot read a compressed zip.
function Expand-Decomp([string]$Py, [string]$Zip, [string]$Manifest, [string]$Srctree) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $wanted = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($line in Get-Content $Manifest) {
        $parts = $line.Split(' ', 3)
        if ($parts.Count -eq 3 -and $parts[0].Length -eq 64) { [void]$wanted.Add($parts[2]) }
    }
    if (Test-Path $DecompDir) { Remove-Item -Recurse -Force $DecompDir }
    [void][IO.Directory]::CreateDirectory($DecompDir)
    $root = [IO.Path]::GetFullPath($DecompDir).TrimEnd($Sep) + $Sep
    Say '   unpacking the source files the build needs...'
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    $count = 0
    try {
        foreach ($e in $archive.Entries) {
            $i = $e.FullName.IndexOf('/')
            if ($i -lt 0) { continue }
            $rel = $e.FullName.Substring($i + 1)
            if (-not $wanted.Contains($rel)) { continue }
            $target = [IO.Path]::GetFullPath((Join-Path $DecompDir ($rel.Replace('/', $Sep))))
            if (-not $target.StartsWith($root)) { Fail "The zip contains an unsafe path: $($e.FullName)" }
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
            $count++
        }
    } finally { $archive.Dispose() }
    Say ("   unpacked {0} of {1} files." -f $count, $wanted.Count)
    $r = Invoke-Native $Py @('-X', 'utf8', '-I', $Srctree, 'verify', '--decomp', $DecompDir, '--manifest', $Manifest)
    if ($r.Code -eq 0) { return $true }
    Say $r.Output 'Yellow'
    return $false
}

function Get-Decomp([string]$Py) {
    Step '4. The game source code'
    $c = $Components.decomp
    $manifest = Join-Path $App 'port\decomp-manifest.txt'
    $commit = ''; $source = ''
    foreach ($line in Get-Content $manifest) {
        if ($line.StartsWith('commit ')) { $commit = $line.Split(' ')[1] }
        if ($line.StartsWith('source ')) { $source = $line.Split(' ')[1] }
    }
    if (-not $commit) { Fail 'app\port\decomp-manifest.txt has no commit line.' }
    Say ("   pinned decomp commit " + $commit.Substring(0, 10) + " (" + $source + ")")
    $srctree = Join-Path $App 'tools\native\srctree.py'
    # an earlier run's unpacked copy, if it still matches every file
    if (Test-Path $DecompDir) {
        $r = Invoke-Native $Py @('-X', 'utf8', '-I', $srctree, 'verify', '--decomp', $DecompDir, '--manifest', $manifest)
        if ($r.Code -eq 0) { Say '   work\decomp from an earlier run: all files match.'; return $DecompDir }
    }
    foreach ($tpl in $c.local_names) {
        $candidate = Join-Path $Downloads ($tpl.Replace('{download_name}', $c.download_name).Replace('{commit}', $commit))
        if (Test-Path $candidate) {
            Say ("   found downloads\" + (Split-Path -Leaf $candidate) + " - unpacking it and checking every file against the manifest...")
            if (Expand-Decomp $Py $candidate $manifest $srctree) { Say '   all files match.'; return $DecompDir }
            Fail ("downloads\" + (Split-Path -Leaf $candidate) + " is not the pinned decomp (see above).")
        }
    }
    if ($Offline) { Fail ("Offline mode, and the game source is not in downloads\. Put $($c.download_name.Replace('{commit}', $commit)) there (from $source/archive/$commit.zip).") }
    $name = $c.download_name.Replace('{commit}', $commit)
    $url = $c.url.Replace('{source}', $source.TrimEnd('/')).Replace('{commit}', $commit)
    $dest = Join-Path $Downloads $name
    Say ("   downloading " + $name + " from github.com (about 43 MB)...")
    Save-Download $url $dest 43500000
    if (-not (Expand-Decomp $Py $dest $manifest $srctree)) { Remove-Item -Force $dest; Fail 'The downloaded source does not match the pinned decomp (it was deleted; see above).' }
    Say '   all files match the manifest.'
    return $DecompDir
}

function Build-Game([string]$Py, [string]$DecompSource) {
    Step '5. Building the game on this computer (a few minutes)'
    if ($Rebuild -and (Test-Path $Build)) { Say '   -Rebuild: removing the previous build.'; Remove-Item -Recurse -Force $Build }
    $env:PATH = (Join-Path $Tc 'bin') + [IO.Path]::PathSeparator + $env:PATH
    $script = Join-Path $App 'tools\native\build_native.py'
    $buildArgs = @('-u', '-X', 'utf8', '-I', $script, '--decomp', $DecompSource, '--profile', 'win32', '--sysroot', (Join-Path $Tc 'i686-w64-mingw32'), '--out', $Build)
    $log = Join-Path $Logs 'build.log'
    $code = Invoke-Logged $Py $buildArgs $log
    $exe = Join-Path $Build 'kyujo.exe'
    if ($code -ne 0 -or -not (Test-Path $exe)) {
        Say ''; Say 'The build stopped. The last lines of its log:' 'Red'
        Get-Content $log -Tail 25 -Encoding UTF8 | ForEach-Object { Say ('   ' + $_) 'Red' }
        Fail "The build failed (exit code $code). Please send the file $log (and $SetupLog) to the developer."
    }
    Say '   checking the finished program...'
    $check = Join-Path $App 'tools\native\pe_check.py'
    $r = Invoke-Native $Py @('-X', 'utf8', '-I', $check, $exe, '--gui', '--befix', '--fragdata')
    ($r.Output -split "`n") | ForEach-Object { Say ('   ' + $_) }
    if ($r.Code -ne 0) { Fail 'The program checks failed (see above). Please send work\logs\setup.log to the developer.' }
    return $exe
}

function Install-Game([string]$Exe, [string]$RomFile) {
    Step '6. Finishing'
    Copy-Item -Force $Exe (Join-Path $Game 'kyujo.exe')
    $cfg = Join-Path $Game 'kyujo.cfg'
    $lines = @()
    if (Test-Path $cfg) { $lines = @(Get-Content $cfg | Where-Object { $_ -notmatch '^\s*game_rom\s*=' }) }
    # the game reads this file as bytes; a path with letters outside plain English may not survive, so a ROM in such a place is copied next to the game
    if ($RomFile -match '[^\x20-\x7E]') {
        $copy = Join-Path $Game 'kyujo-rom.z64'
        if ($RomFile -ne $copy) { Copy-Item -Force -LiteralPath $RomFile $copy }
        $RomFile = 'kyujo-rom.z64'
    }
    $lines = @('game_rom=' + $RomFile) + $lines
    [IO.File]::WriteAllText($cfg, (($lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
    Say ("   game: " + (Join-Path $Game 'kyujo.exe'))
    try {
        $shell = New-Object -ComObject WScript.Shell
        $link = $shell.CreateShortcut((Join-Path $Kit 'Play Kyujo.lnk'))
        $link.TargetPath = Join-Path $Game 'kyujo.exe'
        $link.WorkingDirectory = $Game
        $link.IconLocation = (Join-Path $Game 'kyujo.exe') + ',0'
        $link.Save()
        Say '   shortcut: Play Kyujo.lnk (in this folder)'
    } catch { Say '   (no shortcut could be made; start Game\kyujo.exe instead)' }
}

# ---------------------------------------------------------------------------------------------------------------- main
try {
    [void][IO.Directory]::CreateDirectory($Logs)
    Say ("Kyujo - setup, " + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
    Test-Computer
    $romPath = Select-Rom
    Get-Toolchain
    $py = Find-Python
    Test-Python $py
    $romFile = Confirm-Rom $py $romPath
    $decomp = Get-Decomp $py
    $exe = Build-Game $py $decomp
    Install-Game $exe $romFile
    Say ''
    Say 'Done. Start the game with "Play Kyujo" in this folder (or Game\kyujo.exe).' 'Green'
    Say 'It writes kyujo-log.txt next to the game; send that file with any problem report.'
    if (-not $NoLaunch) {
        $answer = Read-Host 'Start the game now? [Y/n]'
        if ($answer -eq '' -or $answer -match '^[yY]') { Start-Process -FilePath (Join-Path $Game 'kyujo.exe') -WorkingDirectory $Game }
    }
    exit 0
} catch {
    Say ''
    Say ('SETUP STOPPED: ' + $_.Exception.Message) 'Red'
    Say ('Details are in ' + $SetupLog) 'Red'
    exit 1
}
