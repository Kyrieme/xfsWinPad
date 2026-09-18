# _lexer-ids.ps1 - dot-source me. Shared parsers for the E2E harnesses.
# Usage:  . (Join-Path $PSScriptRoot "_lexer-ids.ps1")
#         $styles = Get-StyleMap (Join-Path $RepoRoot "src\language\XfsLexerStyles.h")
#         $lexers = Get-LexerMap (Join-Path $RepoRoot "src\language\XfsLexer.h")
#
# WHY THIS FILE EXISTS
#   The batch-72 copy of ate-langs-e2e.ps1 hardcoded style numbers
#   ($S_ATEP_OPCODE = 66, $S_STIL_BLOCK = 80, ...). Batch 73 renumbered the whole
#   .pat group and renamed most of its styles, and that script silently became
#   wrong - it would either assert against the wrong style id or reference a name
#   that no longer exists. A verification asset that quietly stops matching the
#   thing it verifies is worse than no asset at all, so the numbers are now
#   PARSED from the headers and a missing name is a hard failure.
#
# ASCII only (Windows PowerShell 5.1 reads .ps1 as ANSI unless there is a BOM).

# C-enum evaluator for XfsLexerStyles.h.
# XfsLexerStyles.h is ONE contiguous enum: "kXfsStyleBase = 64," then
# "SCE_ATEP_DEFAULT = kXfsStyleBase," then every following entry implicit. So the
# implicit counter must keep running across the per-group comment banners - do NOT
# reset it on a blank line or on a brace, or SCE_DEC_DEFAULT / SCE_PLN_DEFAULT /
# SCE_STIL_DEFAULT / SCE_ATEL_DEFAULT lose their base and the parsed map is wrong.
# Entries for other enums (StyleRole etc.) are skipped by the SCE_/kXfsStyleBase
# name filter; an unresolvable right-hand side throws instead of guessing.
function Get-StyleMap([string]$path) {
  if (-not (Test-Path $path)) { throw "missing header: $path" }
  $map = @{}
  $cur = $null
  foreach ($raw in (Get-Content -Path $path)) {
    $line = $raw
    $c = $line.IndexOf("//")
    if ($c -ge 0) { $line = $line.Substring(0, $c) }
    $t = $line.Trim()
    if ($t -eq "") { continue }
    if ($t -match '^[A-Za-z0-9_]+\s*=\s*([A-Za-z0-9_]+)\s*,') {
      $name = ($t -split '\s*=\s*')[0].Trim()
      $rhs  = $Matches[1]
      if ($name -notmatch '^(SCE_|kXfsStyleBase)') { continue }
      if ($rhs -match '^\d+$') { $v = [int]$rhs }
      elseif ($map.ContainsKey($rhs)) { $v = $map[$rhs] }
      else { throw "cannot resolve '$rhs' for $name in $path" }
      $map[$name] = $v
      $cur = $v + 1
      continue
    }
    if ($t -match '^(SCE_[A-Z0-9_]+)\s*,') {
      $name = $Matches[1]
      if ($null -eq $cur) { throw "enum entry $name has no starting value" }
      $map[$name] = $cur
      $cur = $cur + 1
      continue
    }
  }
  if ($map.Count -lt 50) { throw "only $($map.Count) SCE_* ids parsed - header layout changed?" }
  return $map
}

# Resolve one style name, failing loudly if the header no longer has it.
function StyleOf([hashtable]$map, [string]$name) {
  if (-not $map.ContainsKey($name)) {
    throw "style '$name' is not in XfsLexerStyles.h - the header moved on and " +
          "this assertion would be meaningless"
  }
  return [int]$map[$name]
}

# name ("ate_pattern", "chroma_plan", ...) -> ILexer5 identifier, from the
# kOwnLexers table in XfsLexer.h. The identifiers must be unique; they are the
# only cross-process-readable proof of which lexer got attached (see the header
# comment in XfsLexer.h), and tests/test_atelexer.cpp asserts that uniqueness.
function Get-LexerMap([string]$path) {
  if (-not (Test-Path $path)) { throw "missing header: $path" }
  $raw = Get-Content -Path $path -Raw
  $names = @{}
  $ids   = @{}
  foreach ($m in [regex]::Matches($raw, 'constexpr\s+const\s+char\*\s+(kLex[A-Za-z0-9_]+)\s*=\s*"([^"]+)"')) {
    $names[$m.Groups[1].Value] = $m.Groups[2].Value }
  foreach ($m in [regex]::Matches($raw, 'constexpr\s+int\s+(kLexId[A-Za-z0-9_]+)\s*=\s*(\d+)')) {
    $ids[$m.Groups[1].Value] = [int]$m.Groups[2].Value }
  $out = @{}
  foreach ($m in [regex]::Matches($raw, '\{\s*(kLex[A-Za-z0-9_]+)\s*,\s*(kLexId[A-Za-z0-9_]+)\s*\}')) {
    $n = $m.Groups[1].Value
    $i = $m.Groups[2].Value
    if (-not $names.ContainsKey($n)) { throw "$n is in kOwnLexers but has no name literal" }
    if (-not $ids.ContainsKey($i))   { throw "$i is in kOwnLexers but has no id value" }
    $out[$names[$n]] = $ids[$i]
  }
  if ($out.Count -lt 5) { throw "kOwnLexers parsed to only $($out.Count) entries" }
  return $out
}
