---
status: accepted
---

# 首期核心支持原子分块追加

Log Source Adapter 可用 Append Batch 为一组曲线原子提交不可变尾部块并生成新 Document Revision，旧数组不复制，LOD 只增量更新受影响尾块；同批曲线整体可见或整体失败。乱序与历史回补必须转为显式替换 Patch，Session 可固定视口或跟随最新深度，高频提交在 C++ 内合并并默认最多每秒触发十次可见刷新。WITSML、MQTT、数据库等协议连接器不进入首期核心。
