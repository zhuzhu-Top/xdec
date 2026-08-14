param([string]$Path)
$c = Get-Content $Path
$body = $c -join "`n"
[PSCustomObject]@{
  lines      = $c.Count
  gotos      = ([regex]::Matches($body, 'goto ')).Count
  temps      = ([regex]::Matches($body, '\bt\d+\b')).Count
  undefLines = ([regex]::Matches($body, 'never set up on this edge')).Count
  unimpl     = ([regex]::Matches($body, '__xdec_unimplemented')).Count
  switches   = ([regex]::Matches($body, 'switch \(')).Count
  labels     = ([regex]::Matches($body, '(?m)^L_0x')).Count
} | Format-List
