# 上位机

上位机接口规范见 [`../docs/protocol.md`](../docs/protocol.md)。

本目录暂时不放实现代码：协议是完整的、自描述的，任何语言按文档实现即可。
`../README.md` 里有一段 Python + pyusb 的最小示例。

如果后续要在本仓库里加参考实现，建议结构：

```
host/
├── ch552spi/            Python 包（pyusb），提供 SpiBridge 类
│   ├── __init__.py
│   ├── device.py        USB 枚举 / bulk 收发
│   ├── protocol.py      组帧 / 解帧 / 命令常量
│   └── flash.py         NOR / NAND 高层操作
├── ch552prog.py         CLI：id / read / write / erase / verify
└── c/                   libusb 版本（可选）
```
