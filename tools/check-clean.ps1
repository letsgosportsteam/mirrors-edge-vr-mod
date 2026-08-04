# Fails if anything that would be committed contains machine-local or personal information.
#
# Run before every push. It scans the git index + working tree (excluding gitignored files),
# so it checks exactly what would actually be published - not what happens to be on disk.
#
# The patterns are deliberately broad. A false positive costs one look; a false negative
# ends up in a public repository's history permanently, where removing it means a rewrite.

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..")

$patterns = @(
    @{ Name = "Windows user profile path"; Rx = '(?i)[A-Z]:\\Users\\[A-Za-z0-9._-]+' }
    @{ Name = "absolute Windows path";     Rx = '(?i)(?<![A-Za-z0-9])[A-Z]:\\[A-Za-z0-9._-]+\\' }
    @{ Name = "UNIX home path";            Rx = '(?i)/(home|Users)/[a-z0-9._-]+/' }
    @{ Name = "email address";             Rx = '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' }
    @{ Name = "GitHub user/repo URL";      Rx = '(?i)github\.com[/:][A-Za-z0-9-]+' }
    @{ Name = "cloud sync folder";         Rx = '(?i)(OneDrive|Dropbox|iCloud Drive|Google Drive)' }
    @{ Name = "Steam/GOG library path";    Rx = '(?i)(SteamLibrary|steamapps)' }
)

# Allowances, each with a reason. Anything not listed here is a finding.
$allow = @(
    @{ Rx = '(?i)noreply@anthropic\.com';          Why = "commit trailer, not a personal address" }
    @{ Rx = '(?i)users\.noreply\.github\.com';     Why = "GitHub privacy address, deliberate" }
    @{ Rx = '(?i)github\.com/(EliotVU|yole|softsoundd|TsudaKageyu)'; Why = "third-party project references" }
    @{ Rx = '(?i)^X:\\path\\';                     Why = "placeholder in paths.local.ps1.example" }
    @{ Rx = '(?i)nvidia\.com';                     Why = "vendor URL in a user-facing message" }
)

$files = git -C $root ls-files --cached --others --exclude-standard
if (-not $files) { throw "no files to check - is this a git repository?" }

$findings = @()
foreach ($rel in $files) {
    $full = Join-Path $root $rel
    if (-not (Test-Path -LiteralPath $full)) { continue }
    # skip binaries
    if ($rel -match '(?i)\.(dll|exe|obj|lib|exp|pdb|ilk|png|jpg|ico|u|upk|xxx)$') { continue }
    # This file necessarily contains every pattern it searches for, so it always matches
    # itself. Skipping it is the only self-exemption here; review it by eye instead.
    if ($rel -match '(?i)tools/check-clean\.ps1$') { continue }
    $text = Get-Content -LiteralPath $full -Raw -ErrorAction SilentlyContinue
    if (-not $text) { continue }
    foreach ($p in $patterns) {
        foreach ($m in [regex]::Matches($text, $p.Rx)) {
            $ok = $false
            foreach ($a in $allow) { if ($m.Value -match $a.Rx) { $ok = $true; break } }
            if (-not $ok) {
                $line = ($text.Substring(0, $m.Index) -split "`n").Count
                $findings += [pscustomobject]@{ File = $rel; Line = $line; Kind = $p.Name; Match = $m.Value }
            }
        }
    }
}

if ($findings) {
    Write-Host ""
    Write-Host "PERSONAL / MACHINE-LOCAL INFORMATION FOUND - do not push:" -ForegroundColor Red
    $findings | Sort-Object File, Line | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
    exit 1
}

Write-Host "clean - $($files.Count) tracked/untracked files scanned, nothing personal found." -ForegroundColor Green
