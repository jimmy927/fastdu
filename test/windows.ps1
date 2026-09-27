# fastdu on Windows, against PowerShell's own view of the same tree: file
# lengths to the byte with -b (counted at each name of a hard-linked file, as
# the listing cannot tell names apart), entries with --inodes, every file with
# -a, and du's other switches by what they must do. There is no GNU du here.
param([Parameter(Mandatory = $true)][string]$Fastdu)
$ErrorActionPreference = 'Stop'

$root = Join-Path ([IO.Path]::GetTempPath()) ("fastdu-test-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path "$root\a\b\c", "$root\empty", "$root\many", "$root\node_modules\pkg" | Out-Null
foreach ($i in 0..2999) { [IO.File]::WriteAllText("$root\many\f$i", "$i") }
[IO.File]::WriteAllBytes("$root\a\one.bin", (New-Object byte[] 200000))
[IO.File]::WriteAllBytes("$root\a\b\c\deep.bin", (New-Object byte[] 50000))
[IO.File]::WriteAllBytes("$root\a\b\linked.bin", (New-Object byte[] 300000))
[IO.File]::WriteAllText("$root\node_modules\pkg\index.js", "x")
New-Item -ItemType HardLink -Path "$root\empty\same.bin" -Target "$root\a\b\linked.bin" | Out-Null
(Get-Item "$root\a\b\c\deep.bin").LastWriteTime = [datetime]'2025-02-03 04:05:06'

$failures = 0
function Fail($what) { Write-Host "FAIL: $what"; $script:failures++ }
function Ok($what) { Write-Host "ok  $what" }
function Lengths($path) {
    [long](Get-ChildItem -LiteralPath $path -Recurse -File -Force | Measure-Object Length -Sum).Sum
}
function Entries($path) {
    1 + [long](Get-ChildItem -LiteralPath $path -Recurse -Force | Measure-Object).Count
}
# fastdu's lines as path -> fields.
function Lines($out) {
    $lines = @{}
    foreach ($line in $out) { $f = $line -split "`t"; $lines[$f[-1]] = $f }
    $lines
}

try {
    foreach ($threads in 1, 8) {
        $lines = Lines (& $Fastdu -j $threads -b $root)
        $good = $true
        foreach ($path in $root, "$root\many", "$root\a\b\c", "$root\a") {
            if ([long]$lines[$path][0] -ne (Lengths $path)) { Fail "-b $path at $threads threads: $($lines[$path][0]), expected $(Lengths $path)"; $good = $false }
        }
        if ($good) { Ok "-b at $threads threads" }
    }

    $lines = Lines (& $Fastdu --inodes $root)
    if ([long]$lines[$root][0] -eq (Entries $root)) { Ok "--inodes" } else { Fail "--inodes: $($lines[$root][0]), expected $(Entries $root)" }

    $lines = Lines (& $Fastdu -b --file-count -s $root)
    # 3000 + one + deep + index.js + the linked file at both names.
    if ($lines[$root][1] -eq '3005') { Ok "--file-count" } else { Fail "--file-count: $($lines[$root][1]), not 3005" }

    $out = & $Fastdu -a -b $root
    if (($out | Measure-Object).Count -eq (Entries $root) - 0) { Ok "-a prints every entry" } else { Fail "-a printed $(($out | Measure-Object).Count) lines, expected $(Entries $root)" }

    $out = & $Fastdu -s $root
    if (($out | Measure-Object).Count -eq 1 -and $out -match "`t$([regex]::Escape($root))$") { Ok "-s" } else { Fail "-s: $out" }

    $out = & $Fastdu -d 1 $root
    if (($out | Measure-Object).Count -eq 5) { Ok "-d 1" } else { Fail "-d 1 printed $(($out | Measure-Object).Count) lines, not 5" }

    $out = & $Fastdu -c -b "$root\a" "$root\many"
    $total = ($out | Select-Object -Last 1) -split "`t"
    if ($total[1] -eq 'total' -and [long]$total[0] -eq (Lengths "$root\a") + (Lengths "$root\many")) { Ok "-c" } else { Fail "-c: $($out | Select-Object -Last 1)" }

    $lines = Lines (& $Fastdu -b -S $root)
    if ([long]$lines["$root\a"][0] -eq 200000) { Ok "-S" } else { Fail "-S: $($lines["$root\a"][0]) for a, not 200000" }

    # The root holds ~861 KB of file lengths, a 550 KB.
    $lines = Lines (& $Fastdu -t 600K -b $root)
    if ($lines.ContainsKey($root) -and -not $lines.ContainsKey("$root\a")) { Ok "-t" } else { Fail "-t 600K kept the wrong folders" }

    $lines = Lines (& $Fastdu -b --exclude=node_modules $root)
    if (-not $lines.ContainsKey("$root\node_modules")) { Ok "--exclude" } else { Fail "--exclude kept node_modules" }

    # -b after -h would turn -h off again, as in du: the units come last.
    $out = & $Fastdu -b -h -s "$root\many"
    if ($out -match '^\d+(\.\d)?K\t') { Ok "-h" } else { Fail "-h: $out" }

    $out = & $Fastdu --time --time-style=iso -a "$root\a\b\c\deep.bin"
    if ($out -match "^\d+`t2025-02-03`t") { Ok "--time" } else { Fail "--time: $out" }

    $lines = Lines (& $Fastdu $root)
    $disk = [long]$lines[$root][0]
    if ($disk -gt 0) { Ok "disk usage by default, in KiB" } else { Fail "default: $disk" }

    # Windows PowerShell stops at a native program's standard error otherwise.
    $ErrorActionPreference = 'Continue'
    & $Fastdu "$root\missing" 2>$null
    if ($LASTEXITCODE -eq 1) { Ok "missing path: exit 1" } else { Fail "a missing path exited $LASTEXITCODE" }
    & $Fastdu --no-such-option $root 2>$null
    if ($LASTEXITCODE -eq 1) { Ok "bad option: exit 1" } else { Fail "a bad option exited $LASTEXITCODE" }
    & $Fastdu -L $root 2>$null
    if ($LASTEXITCODE -eq 1) { Ok "-L refused" } else { Fail "-L exited $LASTEXITCODE" }

    if ($failures -ne 0) { Write-Host "$failures failed"; exit 1 }
    "all passed"
    # Or the script ends with the last run's exit status.
    exit 0
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
