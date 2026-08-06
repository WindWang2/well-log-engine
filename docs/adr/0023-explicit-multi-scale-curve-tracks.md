---
status: accepted
---

# 合并曲线显式绑定 Track Scale

每个 Curve Layer 必须引用稳定的 Track Scale，多曲线可共享或分别使用线性、对数、反向及不同单位的刻度，禁止依据当前可见值临时自动归一化。表头和导出复现每条曲线的名称、颜色、范围、单位和刻度类型；Curve Crossover Fill 在各曲线映射后的图道坐标中求交，以支持 NPHI–RHOB 等异单位和反向刻度组合。默认对超过四组可见刻度给出可读性提示，但内核不硬性禁止。
