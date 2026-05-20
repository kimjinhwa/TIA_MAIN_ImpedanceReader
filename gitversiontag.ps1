# Check if tag option is provided
$createTag = $false
$gitpush = $false
if ($args -contains "-t") {
    $createTag = $true
}
if ($args -contains "-u") {
    $gitpush = $true
}

$currentBranch = git rev-parse --abbrev-ref HEAD 
echo "Current Bransh : $currentBranch "

$content = Get-Content .\Version.h  -Encoding utf8 -TotalCount 1
if ($content -match '#define VERSION "(.*?)".*\/\/(.*)')
{
    $version = $matches[1].Trim()    # "SBMS_1.0.0" 추출
    $comment = $matches[2].Trim()    # "Release Version 1.0.0" 추출
    
    echo "Version: $version"
    echo "Comment: $comment"
    
    # Always commit and push
    git add -A
    git commit -am $comment

    if ($gitpush) {
        git push origin $currentBranch 
    }

    # Create tag only if -t option is provided
    if ($createTag) {
        Write-Host "Creating tag: $version"
        git tag -a $version -m $comment
    } else {
        Write-Host "Tag creation skipped. Use -t option to create a tag."
    }
}
else
{
    echo "Version.h 파일을 찾을 수 없습니다."
}