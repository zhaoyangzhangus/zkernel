# 匹配完整的记录标记；后缀 '=' 有意用于开始一个值。
function Test-LogRecord([string]$Text, [string]$Marker) {
    if ([string]::IsNullOrEmpty($Marker)) { return $false }
    $suffix = if ($Marker.EndsWith('=')) { '' } else { '(?=[ \t\r\n]|$)' }
    return $Text -match ('(?m)(?:^|[ \t])' + [regex]::Escape($Marker) + $suffix)
}
