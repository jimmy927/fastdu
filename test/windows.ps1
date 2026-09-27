# fastdu against PowerShell's own sum of file lengths on a tree with nesting,
# an empty folder, many files in one folder and a file under two names (counted
# at each, as Explorer does): sizes to the byte, on one thread and many.
param([Parameter(Mandatory = $true)][string]$Fastdu)
$ErrorActionPreference = 'Stop'

$root = Join-Path ([IO.Path]::GetTempPath()) ("fastdu-test-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path "$root\a\b\c", "$root\empty", "$root\many" | Out-Null
foreach ($i in 0..2999) { [IO.File]::WriteAllText("$root\many\f$i", "$i") }
[IO.File]::WriteAllBytes("$root\a\one.bin", (New-Object byte[] 200000))
[IO.File]::WriteAllBytes("$root\a\b\c\deep.bin", (New-Object byte[] 50000))
[IO.File]::WriteAllBytes("$root\a\b\linked.bin", (New-Object byte[] 300000))
New-Item -ItemType HardLink -Path "$root\empty\same.bin" -Target "$root\a\b\linked.bin" | Out-Null

function Fail($what) { Write-Error "FAIL: $what"; exit 1 }
function Expected($path) {
    [long](Get-ChildItem -LiteralPath $path -Recurse -File -Force | Measure-Object Length -Sum).Sum
}
function Lines($out) {
    $lines = @{}
    foreach ($line in $out) { $f = $line -split "`t"; $lines[$f[2]] = $f }
    $lines
}

try {
    foreach ($threads in 1, 8) {
        $lines = Lines (& $Fastdu -j $threads $root)
        foreach ($path in $root, "$root\many", "$root\a\b\c") {
            $got = [long]$lines[$path][0]
            if ($got -ne (Expected $path)) { Fail "$path at $threads threads: $got, expected $(Expected $path)" }
        }
        # 3000 + one + deep + the linked file at both names.
        if ($lines[$root][1] -ne '3004') { Fail "$($lines[$root][1]) files at $threads threads, not 3004" }
    }

    $lines = Lines (& $Fastdu -m 150K $root)
    if (-not $lines.ContainsKey("$root\a")) { Fail "-m left out $root\a" }
    if ($lines.ContainsKey("$root\a\b\c")) { Fail "-m kept $root\a\b\c" }

    if ((& $Fastdu -d 1 $root).Count -ne 4) { Fail "-d 1 did not print the root and its three folders" }

    # Windows PowerShell stops at a native program's standard error otherwise.
    $ErrorActionPreference = 'Continue'
    & $Fastdu "$root\missing" 2>$null
    if ($LASTEXITCODE -eq 0) { Fail "a missing path exited 0" }
    & $Fastdu -d x $root 2>$null
    if ($LASTEXITCODE -eq 0) { Fail "a bad depth exited 0" }
    "ok"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
