# Makes the readme for one of the two zips from README.md (used by build_release.bat).
#   installer: README.md as it is (minus the marker lines)
#   portable:  without the installer-only parts, plus the portable-only text (hidden in comments on GitHub)
# README.md marks them like this:
#   <!-- installer-only -->  ...  <!-- /installer-only -->
#   <!-- portable-only
#   ...
#   -->
param(
    [Parameter(Mandatory)][ValidateSet('installer', 'portable')][string]$Variant,
    [Parameter(Mandatory)][string]$In,
    [Parameter(Mandatory)][string]$Out
)
$t = [IO.File]::ReadAllText($In) -replace "`r`n", "`n"
if ($Variant -eq 'portable') {
    $t = [regex]::Replace($t, '(?s)<!-- installer-only -->\n.*?<!-- /installer-only -->\n', '')
    $t = [regex]::Replace($t, '(?s)<!-- portable-only\n(.*?)-->\n', '$1')
} else {
    $t = [regex]::Replace($t, '(?m)^<!-- /?installer-only -->\n', '')
    $t = [regex]::Replace($t, '(?s)<!-- portable-only\n.*?-->\n', '')
}
if ($t -match '<!--') { throw "unbalanced readme markers in $In" }
[IO.File]::WriteAllText($Out, ($t -replace "`n", "`r`n"))
