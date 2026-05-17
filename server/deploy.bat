@echo off
setlocal
set SERVER=192.168.11.38
set REMOTE=/opt/raa-server
set NODE=/home/YOUR_USER/.nvm/versions/node/v24.x.x/bin/node
set SCP="C:\Program Files\Git\usr\bin\scp.exe"
set SSH="C:\Program Files\Git\usr\bin\ssh.exe"
set ROOT=%~dp0

echo Deploying to %SERVER% ...

%SCP% "%ROOT%package.json"      %SERVER%:%REMOTE%/package.json      || goto :err
%SCP% "%ROOT%src\main.js"       %SERVER%:%REMOTE%/src/main.js       || goto :err
%SCP% "%ROOT%src\mixer.js"      %SERVER%:%REMOTE%/src/mixer.js      || goto :err
%SCP% "%ROOT%src\udp.js"        %SERVER%:%REMOTE%/src/udp.js        || goto :err
%SCP% "%ROOT%src\clients.js"    %SERVER%:%REMOTE%/src/clients.js    || goto :err
%SCP% "%ROOT%public\index.html"             %SERVER%:%REMOTE%/public/index.html             || goto :err
%SCP% "%ROOT%public\opus-decoder.bundle.js" %SERVER%:%REMOTE%/public/opus-decoder.bundle.js || goto :err

echo Restarting raa-server ...
%SSH% %SERVER% "kill $(ps aux | grep 'node src/main.js' | grep -v grep | awk '{print $2}') 2>/dev/null; sleep 1; cd %REMOTE% && nohup %NODE% src/main.js >> /tmp/raa.log 2>&1 & disown && echo started"

echo Done.
goto :eof

:err
echo Deploy failed.
exit /b 1
