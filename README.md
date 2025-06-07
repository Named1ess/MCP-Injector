1 将MCP_Tool.cpp编译为MCP_Tool.dll \
2 将Injector.cpp编译为Injector.exe \
3 MCP_Tool.dll和Injector.exe放在同一路径下 \
4 按照https://github.com/jlowin/fastmcp 的教程安装FastMCP \
5 在FastMCP环境下粘贴本库中的main.py \
6 dll注入方式: 命令行下输入 Injector.exe PID \
7启动main.py \
8 用sse连接8000端口即可 \
与LLM交互：用MCP工具向目标程序写入6666 
