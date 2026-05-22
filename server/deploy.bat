@echo off
setlocal
set SERVER=192.168.11.38
set REMOTE=/home/daigo/raa-server
set NODE=/home/daigo/.nvm/versions/node/v24.15.0/bin/node
set SCP="C:\Program Files\Git\usr\bin\scp.exe"
set SSH="C:\Program Files\Git\usr\bin\ssh.exe"
set ROOT=%~dp0

echo Deploying to %SERVER% ...

%SCP% "%ROOT%package.json"      %SERVER%:%REMOTE%/package.json      || goto :err
%SCP% "%ROOT%src\main.js"       %SERVER%:%REMOTE%/src/main.js       || goto :err
%SCP% "%ROOT%src\mixer.js"      %SERVER%:%REMOTE%/src/mixer.js      || goto :err
%SCP% "%ROOT%src\udp.js"        %SERVER%:%REMOTE%/src/udp.js        || goto :err
%SCP% "%ROOT%src\clients.js"    %SERVER%:%REMOTE%/src/clients.js    || goto :err
%SCP% "%ROOT%src\ogg-reader.js" %SERVER%:%REMOTE%/src/ogg-reader.js || goto :err
%SSH% %SERVER% "mkdir -p %REMOTE%/assets"
%SCP% "%ROOT%assets\goldberg-var1.opus" %SERVER%:%REMOTE%/assets/goldberg-var1.opus || goto :err
%SSH% %SERVER% "mkdir -p %REMOTE%/bench"
%SCP% "%ROOT%bench\load-test.js" %SERVER%:%REMOTE%/bench/load-test.js || goto :err
%SCP% "%ROOT%public\index.html"             %SERVER%:%REMOTE%/public/index.html             || goto :err
%SCP% "%ROOT%public\opus-decoder.bundle.js" %SERVER%:%REMOTE%/public/opus-decoder.bundle.js || goto :err

echo Restarting raa-server ...
%SSH% %SERVER% "systemctl --user restart raa-server && echo started"

echo Done.
goto :eof

:err
echo Deploy failed.
exit /b 1
