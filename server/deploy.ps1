# deploy.ps1 - ローカルの server/ をサーバーに転送して再起動する
$Server    = "192.168.11.38"
$RemoteDir = "/opt/raa-server"
$scp       = "C:\Program Files\Git\usr\bin\scp.exe"
$ssh       = "C:\Program Files\Git\usr\bin\ssh.exe"
$root      = Split-Path -Parent $MyInvocation.MyCommand.Path

$files = @(
    @{ local = "$root\package.json";       remote = "${Server}:${RemoteDir}/package.json" },
    @{ local = "$root\src\main.js";        remote = "${Server}:${RemoteDir}/src/main.js" },
    @{ local = "$root\src\mixer.js";       remote = "${Server}:${RemoteDir}/src/mixer.js" },
    @{ local = "$root\src\udp.js";         remote = "${Server}:${RemoteDir}/src/udp.js" },
    @{ local = "$root\src\clients.js";     remote = "${Server}:${RemoteDir}/src/clients.js" },
    @{ local = "$root\public\index.html";  remote = "${Server}:${RemoteDir}/public/index.html" }
)

Write-Host "Deploying to $Server ..."
foreach ($f in $files) {
    Write-Host "  $(Split-Path $f.local -Leaf)"
    & $scp $f.local $f.remote
    if ($LASTEXITCODE -ne 0) { Write-Error "scp failed: $($f.local)"; exit 1 }
}

Write-Host "Restarting raa-server ..."
& $ssh $Server "source ~/.nvm/nvm.sh; pkill -f 'node src/main.js' 2>/dev/null; sleep 1; cd $RemoteDir && nohup node src/main.js > /tmp/raa.log 2>&1 & echo started"

Write-Host "Done. Log: ssh $Server 'tail -f /tmp/raa.log'"
