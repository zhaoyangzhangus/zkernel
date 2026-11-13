# 只规范化对象列表数据；不要重写空行或注释。
function Test-ObjectListData([string]$Path) {
    return (-not [string]::IsNullOrWhiteSpace($Path) -and
            -not $Path.TrimStart().StartsWith('#'))
}

function ConvertTo-ObjectListPath([string]$Path) {
    if (-not (Test-ObjectListData $Path)) {
        return $Path
    }
    return $Path.Replace('\', '/')
}
