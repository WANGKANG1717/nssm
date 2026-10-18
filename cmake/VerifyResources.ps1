param(
  [Parameter(Mandatory = $true)]
  [string]$Executable
)

$bytes = [System.IO.File]::ReadAllBytes($Executable)
$utf16 = [System.Text.Encoding]::Unicode.GetString($bytes)

$expected = @(
  'NSSM 服务安装器',
  '服务名称：',
  '输出 (stdout)：',
  '替换已存在的输出/错误文件',
  '重定向钩子的输出'
)

foreach ($text in $expected) {
  if (-not $utf16.Contains($text)) {
    throw "Missing expected UTF-16 resource string: $text"
  }
}

$mojibake = @('鏈嶅姟', '鍚嶇О', '閽╁瓙')
foreach ($text in $mojibake) {
  if ($utf16.Contains($text)) {
    throw "Found mojibake in compiled resources: $text"
  }
}
