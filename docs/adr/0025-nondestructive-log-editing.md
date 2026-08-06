---
status: accepted
---

# 解释可编辑而原始曲线不可变

图道布局、Track Scale、样式、可见性、层位、区间、注释、跨井连接和 Depth Transform 控制点可通过可撤销的 Document Patch 编辑；原始曲线数组不可原地修改。异常样点使用 QC Mask 标记，平滑、滤波、重采样和单位换算产生记录来源与参数的 Derived Curve。内核维护会话撤销栈并发布提交事件，宿主负责持久化，同时明确区分未提交修改、Document Revision 与项目保存状态。
