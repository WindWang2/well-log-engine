---
status: accepted
---

# 外部数据与扩展资产均视为不可信输入

缓冲区长度、步长、偏移和字节计算必须检查溢出，Manifest、XML、XLSX、字体、图纹、图像和便携包设置尺寸、嵌套、对象数量、压缩比与展开量上限，并阻止路径穿越；XML 禁用外部实体和网络资源。Pattern Definition 与 Custom Layer 不接受脚本、Shader 或命令，图片按限制解码。错误隔离到当前对象或文档，解析器持续 fuzz，日志和崩溃报告默认不包含井数据内容。
