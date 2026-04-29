**宿主机上先启动一个临时监听服务**

meteor@DESKTOP-E75SD7O:~/bar$ python3 -m http.server 8080 --bind 10.0.0.1
Serving HTTP on 10.0.0.1 port 8080 (http://10.0.0.1:8080/) ...
10.0.0.2 - - [29/Apr/2026 11:36:29] "GET / HTTP/1.1" 200 -


容器里：
```
# curl -v http://10.0.0.1:8080
*   Trying 10.0.0.1:8080...
* Connected to 10.0.0.1 (10.0.0.1) port 8080
> GET / HTTP/1.1
> Host: 10.0.0.1:8080
> User-Agent: curl/8.5.0
> Accept: */*
> 
* HTTP 1.0, assume close after body
< HTTP/1.0 200 OK
< Server: SimpleHTTP/0.6 Python/3.12.3
< Date: Wed, 29 Apr 2026 03:36:29 GMT
< Content-type: text/html; charset=utf-8
< Content-Length: 581
< 
<!DOCTYPE HTML>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Directory listing for /</title>
</head>
<body>
<h1>Directory listing for /</h1>
<hr>
<ul>
<li><a href=".clang-format">.clang-format</a></li>
<li><a href=".clang-tidy">.clang-tidy</a></li>
<li><a href=".vscode/">.vscode/</a></li>
<li><a href="bin/">bin/</a></li>
<li><a href="build/">build/</a></li>
<li><a href="include/">include/</a></li>
<li><a href="lib/">lib/</a></li>
<li><a href="Makefile">Makefile</a></li>
<li><a href="src/">src/</a></li>
<li><a href="tests/">tests/</a></li>
</ul>
<hr>
</body>
</html>
* Closing connection
# 
```

