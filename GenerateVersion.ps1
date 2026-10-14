# Writes version.h from the last tag (v2.0.0 -> 2.0.0.<commits since the tag>).
# The file is only rewritten when its content changes, so a build without a new
# commit does not recompile the resources.
param (
    [string]$outPath = "version.h"
)

$major, $minor, $patch, $build = 2, 0, 0, 0
$suffix = if ($env:GITHUB_ACTIONS -eq "true") { "" } else { "-local" }
$describe = git describe --tags --long --match "v*" 2>$null
if ($describe -match '^v(\d+)\.(\d+)\.(\d+)-(\d+)-g[0-9a-f]+$') {
    $major, $minor, $patch, $build = $Matches[1], $Matches[2], $Matches[3], $Matches[4]
}
$years = "2016-$((Get-Date).Year)"

$content = @"
#ifndef VERSION_H
#define VERSION_H
#define VER_FILE_VERSION          $major,$minor,$patch,$build
#define VER_PRODUCT_VERSION       $major,$minor,$patch,$build
#define VER_FILE_VERSION_STR      "$major.$minor.$patch.$build\0"
#define VER_PRODUCT_VERSION_STR   "$major.$minor.$patch.$build$suffix\0"
#define VER_COPYRIGHT_YEARS_STR   "$years"
#define VER_LEGAL_COPYRIGHT_STR   "Copyright (C) $years MySmartLogon"
#endif // VERSION_H

"@

$path = [System.IO.Path]::GetFullPath($outPath)
if (-not (Test-Path $path) -or [System.IO.File]::ReadAllText($path) -ne $content) {
    [System.IO.File]::WriteAllText($path, $content)
}
