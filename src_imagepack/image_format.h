/*
 * P0-2 结构化镜像容器格式（路线 A 仿真闭环口径，冻结自 4f §5.4）。
 *
 * 32 位字容器，4 字节/字，小端字节流（对应外挂 SPI NOR 字节编址）。
 * 数据字：bit31 预留=0，bit30 符号位，bit29:0 数值域（定点 1.30，符号-数值码）。
 * 控制字（目录头/段表项）不套用数据字符号位语义，按字段打包。
 *
 * 布局（字序）：
 *   目录头  字0 MAGIC；字1 N / entry PC / 版本·算法标识
 *   段表    每段 2 字（A/B）
 *   镜像体  各段 32 位字按段顺序紧凑排列
 *   校验尾  整体累加和（覆盖目录头 + 段表 + 镜像体，不含校验尾本身）
 *
 * 本头文件被打包器（tape-converter/src_imagepack）与模拟器装载侧
 * （simulator/src_c）各持一份副本，两侧必须保持一致。
 */
#ifndef IMAGE_FORMAT_H
#define IMAGE_FORMAT_H

#include <stdint.h>

/* 魔数：31 位固定识别值（bit31 预留 0）。 */
#define IMG_MAGIC          0x103103u

/* 目录头字1：版本/算法标识字段 [30:24]。 */
#define IMG_VERSION        1u
#define IMG_ALGO_SUM       0u   /* 按字累加和（mod 2^31，符号位单独累加）——本轮实现 */
#define IMG_ALGO_CRC16     1u   /* 预留：上板阶段升级 CRC-16，目录结构不变 */

#define IMG_MAX_SEG        16u   /* 段数工程上限 */
#define IMG_ADDR_MASK      0xfffu
#define IMG_WORDS          4096u /* 12 位地址空间字数 */
#define IMG_VALUE_MASK     0x3fffffffu
#define IMG_SIGN_BIT       0x40000000u
/* 校验和累加掩码：mod 2^31（保留 bit30 符号位空间；bit31 预留恒 0）。
 * 注意区别于 IMG_VALUE_MASK（数据字低 30 位数值域掩码）；校验和两侧必须同口径。 */
#define IMG_SUM_MASK       0x7fffffffu

/* 段类型枚举（不设栈段）。 */
#define SEG_PROG           0u   /* 程序段 */
#define SEG_DATA           1u   /* 常量 / 数据段 */
#define SEG_RSV            2u   /* 保留（装载侧报错） */
#define SEG_TYPE_MAX       2u

/* ---- 目录头字1 打包/解包 ---- */
/* N[11:0] | entry[23:12] | ver_algo[30:24] */
static inline uint32_t img_pack_hdr1(uint32_t n, uint32_t entry, uint32_t ver_algo) {
    return (n & IMG_ADDR_MASK)
         | ((entry & IMG_ADDR_MASK) << 12)
         | ((ver_algo & 0x7fu) << 24);
}
static inline uint32_t img_hdr1_n(uint32_t w)          { return w & IMG_ADDR_MASK; }
static inline uint32_t img_hdr1_entry(uint32_t w)     { return (w >> 12) & IMG_ADDR_MASK; }
static inline uint32_t img_hdr1_ver_algo(uint32_t w)   { return (w >> 24) & 0x7fu; }

/* ---- 段表项打包/解包 ----
 * A: dst[11:0] | body 内字索引 off[23:12] | 类型[26:24] | 标志[30:27]。
 * 路线 A 单镜像闭环：body 紧凑排列，off 为段首字在镜像体内的 0 起始字索引
 * （定位时 ×4 即字节偏移）。24 位绝对 NOR 字节偏移留待路线 B：上板一颗 NOR
 * 放多份镜像时，段表项扩为第三字再承载绝对偏移，两侧打包/装载口径不变。
 */
static inline uint32_t img_pack_seg_a(uint32_t dst, uint32_t off,
                                      uint32_t type, uint32_t flags) {
    return (dst & IMG_ADDR_MASK)
         | ((off & IMG_ADDR_MASK) << 12)
         | ((type & 0x7u) << 24)
         | ((flags & 0xfu) << 27);
}
static inline uint32_t img_seg_a_dst(uint32_t w)    { return w & IMG_ADDR_MASK; }
static inline uint32_t img_seg_a_off(uint32_t w)    { return (w >> 12) & IMG_ADDR_MASK; }
static inline uint32_t img_seg_a_type(uint32_t w)   { return (w >> 24) & 0x7u; }
static inline uint32_t img_seg_a_flags(uint32_t w)  { return (w >> 27) & 0xfu; }

/* B: 字数 len[11:0]，预留高位。 */
static inline uint32_t img_pack_seg_b(uint32_t len) { return len & IMG_ADDR_MASK; }
static inline uint32_t img_seg_b_len(uint32_t w)    { return w & IMG_ADDR_MASK; }

/* ---- 校验和 ----
 * 每个参与字贡献：数值域低 30 位 + 符号位 1 位；累加后 mod 2^31。
 * 控制字无符号位语义，统一取低 31 位，公式两侧一致即可。
 */
static inline uint32_t img_sum_word(uint32_t w) {
    return (w & IMG_VALUE_MASK) + ((w >> 30) & 1u);
}

/* 小端 32 位读写（显式拆字节，不依赖平台端序）。 */
static inline void img_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}
static inline uint32_t img_get_le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

#endif /* IMAGE_FORMAT_H */
