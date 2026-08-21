---
status: accepted
---

# LIS79 导入的 ResForm 兼容 v1 身份与规则快照

LIS79 导入以一个**不可变的规则快照**(`resform-compatible-v1`)定义曲线别名归一、规范单位与缺失值哨兵;该快照同时参与**导入身份**(document/axis/curve identity)计算。行为改变必须通过新的 profile 版本,绝不原地修改 v1 数据。

## 背景

workbench 侧的领域词汇(ResForm Compatibility Model / Log Normalization Profile,见 paleo-workbench CONTEXT.md)要求:导入后的深度、曲线、单位、空值语义符合解释人员预期;归一化配置拥有可审计的稳定指纹;相同内容及规则重复导入保持同一身份,内容或规则变化则形成新文档。WellLogEngine 的 LIS79 读取器(`src/io/lis.cpp`)负责把 LIS 物理记录流解析为标准文档,同样需要一套与宿主一致的别名/单位规则。

实现历史中,别名/单位规则曾以可执行代码表(`builtin_depth_unit_rules`、映射函数)与"当前行为摘要"的形式存在;导入身份随之隐式漂移——同一文件在不同引擎版本下可能得到不同身份,违反身份稳定性要求。

## 决策

1. **不可变协议数据与可执行规则表分离**:`src/io/lis.cpp` 中的 `resform_compatible_v1_identity_component`(曲线别名分组 + 单位族 + 深度单位换算,如 `GR=GR,GAM,GAMMA;...;AC/us/m,us/ft;DEPTH/m,ft,in`)是**已持久化 wire format 的描述**,与可再生的行为摘要表分开存放。它只描述历史版本,永不改写。
2. **行为变化 ⇒ 新 profile 版本**:任何别名/单位/哨兵规则的变更必须新增版本号(v2、v3…),旧版本字符串永久保留以读取既有持久化身份。
3. **身份 preimage 包含规则指纹**:LIS 文件内容指纹 + 逻辑数据集身份 + 规则组件字符串共同构成 entity-identity preimage;规则不变时同一文件重复导入得到同一身份与初始 Revision。
4. **深度单位换算表是协议的一部分**:`M=1.0 / FT=0.3048 / IN=0.0254` 与别名表同级,同样不可变。

## 后果

- 引擎侧 LIS 导入身份与宿主侧 Imported Log Identity 语义对齐:规则变化产生新文档而非覆盖。
- 增加规则时的成本 = 新增常量表 + profile 版本号,不能通过改函数逻辑"顺手"完成。
- 单元测试必须对每个存续 profile 版本固化身份字符串(防止意外编辑协议数据)。

## 关联

- paleo-workbench CONTEXT.md:`Log Normalization Profile` / `Imported Log Identity` / `ResForm-Compatible v1 Aliases/Units`
- `src/io/lis.cpp`:`resform_compatible_v1_identity_component`、`builtin_depth_unit_rules`
- #886 之前的审计轮(G2/GVE#75)确认双解析器行为一致性以该快照为基准
