---
status: accepted
---

# XML 使用版本化测井表格 Schema

XML 导出使用带 XSD 的测井交换结构，记录 schema 版本、井身份、Reference Depth、列 Entity ID、单位、类型、Sampling Axis 与 Null 规则；原始表、Resampled Table、区间、Marker 和注释具有明确结构并流式写出。XLSX 单独承担 Excel 兼容并在超过工作表容量时连续分表，XML/XLSX 均支持完整 Table Projection 或仅 Selection Set；SpreadsheetML 不作为首期 XML 语义。
