# Text-level counts of the redundant-intermediate-variable shapes
# docs/09-expression-reuse.md and docs/14-emit-redundancy.md classify, taken
# straight from a printed .c file.
#
# Complements analysis::EmitRedundancyReport (`xdec decompile --emit-report`)
# rather than duplicating it: that report counts what the IL and
# VariableTable can prove on their own; this counts what the emitter's own
# per-scope CSE and declaration choices actually printed, which is not an IL
# fact and has no cheaper way to ask than reading the text. Run this before
# and after a phase's change on the same function to see its effect.
param(
  [Parameter(Mandatory = $true)][string]$Path
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Path)) {
  throw "not found: $Path"
}

$lines = Get-Content $Path
$text = $lines -join "`n"

# A local's own declaration line (`uint32_t* var_980; // sp-2432`) is not a
# use of the variable, so it is excluded before the per-name occurrence scan
# below; left in, every local would look "read" once just for being declared.
$bodyLines = $lines | Where-Object { $_ -notmatch '^\s*\S+\s+var_[0-9a-f]+;' }
$bodyText = $bodyLines -join "`n"

$report = [ordered]@{
  file            = $Path
  lines           = $lines.Count
  # Shape I3 (and whatever I1/I2 has not yet folded away): every CSE temp
  # materialization.
  cse_assigns     = ([regex]::Matches($text, '(?m)^\s*_cse\d+\s*=')).Count
  # Shape H1/H2: a CSE temp immediately spilled into a stack local.
  var_from_cse    = ([regex]::Matches($text, '(?m)^\s*var_[0-9a-f]+\s*=\s*_cse\d+;')).Count
  # Shape F's remainder: a stack local re-read into a plain temp.
  t_from_var      = ([regex]::Matches($text, '(?m)^\s*t\d+\s*=\s*var_[0-9a-f]+;')).Count
  # Shape G: a non-stack-slot memory read materialized into a plain temp.
  t_from_load     = ([regex]::Matches($text, '(?m)^\s*t\d+\s*=\s*\(\*\(')).Count
  write_only_vars = 0
  # Shape I2: distinct `_cseN = <expr>;` right-hand sides that recur under a
  # different _cseN elsewhere in the file -- each scope's own beginScope
  # forgets every name the previous scope minted, so an obfuscator's MBA
  # round computed identically in several mutually-exclusive switch arms
  # gets renamed and recomputed once per arm rather than recognised as the
  # same value. Approximate on purpose: this groups by the RHS's literal
  # printed text, not by proven cross-scope equivalence, so a hoist would
  # still need per-site safety proof (docs/09 shape D) this count does not
  # attempt -- it exists to size the opportunity, not to justify a rewrite.
  duplicate_cse_rhs_groups      = 0
  duplicate_cse_rhs_occurrences = 0
}

$varNames = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($m in [regex]::Matches($bodyText, '\bvar_[0-9a-f]+\b')) {
  [void]$varNames.Add($m.Value)
}
foreach ($name in $varNames) {
  $escaped = [regex]::Escape($name)
  $all = ([regex]::Matches($bodyText, "\b$escaped\b")).Count
  # An assignment target: the name starts the (trimmed) statement and is
  # followed by `=` that is not `==`. Every other occurrence -- an operand, a
  # `&name`, the address side of a compound assignment -- is a read.
  $assigns = ([regex]::Matches($bodyText, "(?m)^\s*$escaped\s*=(?!=)")).Count
  if ($all -gt 0 -and $all -eq $assigns) {
    $report.write_only_vars++
  }
}

$cseRhsCounts = @{}
foreach ($m in [regex]::Matches($text, '(?m)^\s*_cse\d+\s*=\s*(.+);\s*$')) {
  $rhs = $m.Groups[1].Value
  if ($cseRhsCounts.ContainsKey($rhs)) { $cseRhsCounts[$rhs]++ } else { $cseRhsCounts[$rhs] = 1 }
}
foreach ($count in $cseRhsCounts.Values) {
  if ($count -ge 2) {
    $report.duplicate_cse_rhs_groups++
    $report.duplicate_cse_rhs_occurrences += $count
  }
}

[PSCustomObject]$report | Format-List
