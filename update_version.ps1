# Get the current commit hash
$commitHash = git rev-parse HEAD | ForEach-Object { $_.Trim() }

# Read version.h
$versionFile = "version.h"
$content = Get-Content $versionFile -Raw

# Find and replace the COMMIT_SHA
$newContent = $content -replace '(#define COMMIT_SHA ")[^"]*(")', ('$1' + $commitHash + '$2')

# Write the new content back to version.h
Set-Content -Path $versionFile -Value $newContent -NoNewline

# Stage the updated version.h
git add $versionFile
