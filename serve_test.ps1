# 本地静态服务器, 用于测试 test_ai.html (无 python/node 环境)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$port = 8765
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://127.0.0.1:$port/")
$listener.Start()
Write-Host "serving $root at http://127.0.0.1:$port/test_ai.html"
$mime = @{ '.html'='text/html'; '.js'='text/javascript'; '.wasm'='application/wasm';
           '.onnx'='application/octet-stream'; '.jpg'='image/jpeg'; '.png'='image/png' }
while ($listener.IsListening) {
  $ctx = $listener.GetContext()
  $p = [System.Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath.TrimStart('/'))
  if (-not $p) { $p = 'test_ai.html' }
  $f = Join-Path $root $p
  if (Test-Path $f -PathType Leaf) {
    $ext = [System.IO.Path]::GetExtension($f)
    $ctx.Response.ContentType = $(if ($mime[$ext]) { $mime[$ext] } else { 'application/octet-stream' })
    $bytes = [System.IO.File]::ReadAllBytes($f)
    $ctx.Response.ContentLength64 = $bytes.Length
    $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
  } else {
    $ctx.Response.StatusCode = 404
  }
  $ctx.Response.Close()
}
