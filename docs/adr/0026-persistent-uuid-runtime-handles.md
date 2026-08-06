---
status: accepted
---

# 持久 UUID 与运行时 Handle 分层

井、Sampling Axis、曲线、Track、Layer、区间、层位、注释及跨井对象使用持久 128 位 UUID，禁止以名称、数组地址或列表序号充当身份；Log Source Adapter 可从稳定源身份确定性生成 UUID，新解释对象由内核生成。会话内部将 UUID 映射为紧凑 32 位 Runtime Handle 供批次、索引和拾取使用，但 Handle 不进入持久化或 Python 业务接口。Document Patch 同时引用 UUID 与基础 Document Revision，版本不匹配时显式报告冲突。
