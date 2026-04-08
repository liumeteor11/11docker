参考资料：
[veggiemonk/awesome-docker: :whale: A curated list of Docker resources and projects](https://github.com/veggiemonk/awesome-docker)

[Build your own Docker | CodeCrafters](https://app.codecrafters.io/courses/docker/overview)
[codecrafters-io/build-your-own-x: Master programming by recreating your favorite technologies from scratch.](https://github.com/codecrafters-io/build-your-own-x)

[Linux containers in 500 lines of code](https://blog.lizzie.io/linux-containers-in-500-loc.html)
[barco: Linux Containers From Scratch in C. | Blog | Luca Cavallin](https://www.lucavall.in/blog/barco-linux-containers-from-scratch-in-c)

![](assets/basis/file-20260407144259420.png)

| 场景                            | 推荐方案       | 原因                                     |
| ----------------------------- | ---------- | -------------------------------------- |
| 数据库数据（PostgreSQL/MySQL/Redis） | Volume     | 需要高 IO 性能、权限隔离、Docker 自动备份/迁移          |
| 应用运行时产生的日志/缓存                 | Volume     | 容器专属，不污染宿主机目录结构                        |
| 开发时修改源代码                      | Bind Mount | `./src:/app/src`，改完热重载，无需重新 build      |
| 配置文件（nginx.conf / .env）       | Bind Mount | 宿主机直接编辑，容器实时读取                         |
| 需要宿主机工具查看/备份的数据               | Bind Mount | 路径明确，可用 `robocopy` / `rsync` / 压缩包直接操作 |

1.简单了解 docker的功能和用法

[furkan/dockerlings: learn docker in your terminal, with bite sized exercises](https://github.com/furkan/dockerlings)
打算直接把这个做一遍
core-01:![](assets/basis/file-20260408010346223.png)
直接修改echo后面的内容就行
```docker file
# Use the emptiest possible base image
FROM docker.1ms.run/library/alpine:latest

# TODO: Fix this command to output "Hello Docker"
CMD ["echo", "Hello Docker"]

```
然后`docker build -t hello-docker .`(-t指定名称)、`docker run --rm hello-docker`(直接--rm，容器退出后自动删除该容器)
