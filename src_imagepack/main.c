/*
 * image-pack：P0-2 结构化镜像打包器（tape-converter 家族）。
 *
 * 输入：段定义文件（.seg，八进制）+ 扁平 .103 文本（地址-值行）。
 * 输出：结构化镜像 .bin（目录头 + 段表 + 镜像体 + 校验尾，小端 32 位字），
 *       可选派生 .lst 十六进制文本清单（供 RTL $readmemh / diff 冻结）。
 *
 * 用法：image-pack <def.seg> <input.103> -o <image.bin> [--lst <image.lst>]
 *
 * .seg 行格式（八进制）：
 *   entry <entry_pc>
 *   seg <type> <dst> <src_lo> <src_hi>
 *     type 0=程序段 1=常量/数据段 2=保留；src_lo/src_hi 为 .103 内地址区间（含），
 *     段内连续；dst 为装载目的主存起始地址。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <getopt.h>

#include "image_format.h"

typedef struct {
    uint32_t type;
    uint32_t dst;
    uint32_t src_lo;
    uint32_t src_hi;
    uint32_t len;
} seg_def_t;

/* 扁平 .103 字典：addr -> 32 位干净字；present 标记是否已填。 */
static uint32_t dict_mem[IMG_WORDS];
static int      dict_present[IMG_WORDS];

static void die(const char *msg) {
    fprintf(stderr, "image-pack: %s\n", msg);
    exit(EXIT_FAILURE);
}

static uint32_t parse_oct(const char *s) {
    return (uint32_t)strtoul(s, NULL, 8) & IMG_ADDR_MASK;
}

/* 解析一行 .103，填入字典。返回 0 成功，-1 空行/注释跳过。 */
static int parse_103_line(char *line) {
    char *at = strchr(line, '@');
    if (!at) return -1;

    char addrbuf[5] = {0};
    int k = 0;
    for (char *p = line; p < at && k < 4; p++) {
        if (isdigit((unsigned char)*p)) addrbuf[k++] = *p;
    }
    if (k == 0) return -1;
    uint32_t addr = parse_oct(addrbuf);

    char *q = at + 1;
    while (isspace((unsigned char)*q)) q++;
    int neg = 0;
    if (*q == '-') { neg = 1; q++; }
    else if (*q == '+') { q++; }
    while (isspace((unsigned char)*q)) q++;

    /* 切分剩余字段 token（最多 3：op c1 c2；或 1：纯数值）。 */
    char *tok[3];
    int nt = 0;
    char *save = NULL;
    for (char *t = strtok_r(q, " \t\r\n", &save); t && nt < 3;
         t = strtok_r(NULL, " \t\r\n", &save)) {
        tok[nt++] = t;
    }
    if (nt == 0) return -1;

    uint32_t word;
    if (nt == 3) {
        uint32_t op  = strtoul(tok[0], NULL, 8) & 0x3fu;
        uint32_t c1  = parse_oct(tok[1]);
        uint32_t c2  = parse_oct(tok[2]);
        word = (op << 24) | (c1 << 12) | c2;
    } else {
        uint32_t val = strtoul(tok[0], NULL, 8) & IMG_VALUE_MASK;
        word = val;
    }
    if (neg) word |= IMG_SIGN_BIT;

    dict_mem[addr] = word;
    dict_present[addr] = 1;
    return 0;
}

static void load_103(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { char b[256]; snprintf(b, sizeof b, "打不开 %s", path); die(b); }
    char line[160];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        parse_103_line(line);
    }
    fclose(f);
}

static void parse_def(const char *path, seg_def_t *segs, int *nseg, uint32_t *entry) {
    FILE *f = fopen(path, "r");
    if (!f) { char b[256]; snprintf(b, sizeof b, "打不开 %s", path); die(b); }
    char line[160];
    int n = 0;
    *entry = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;

        char kw[16];
        if (sscanf(p, "%15s", kw) != 1) continue;

        if (strcmp(kw, "entry") == 0) {
            unsigned long e;
            if (sscanf(p, "%*s %lo", &e) != 1) die("entry 格式应为 entry <八进制地址>");
            *entry = (uint32_t)e & IMG_ADDR_MASK;
        } else if (strcmp(kw, "seg") == 0) {
            unsigned long type, dst, lo, hi;
            if (sscanf(p, "%*s %lo %lo %lo %lo", &type, &dst, &lo, &hi) != 4)
                die("seg 格式应为 seg <类型> <目的> <源起> <源止>");
            if (n >= (int)IMG_MAX_SEG) die("段数超过上限 16");
            if (type > SEG_TYPE_MAX) die("段类型非法");
            if (type == SEG_RSV) die("保留类型(2)不能出现在打包定义中");
            if (lo > hi) die("段源起地址大于源止地址");
            segs[n].type   = (uint32_t)type;
            segs[n].dst    = (uint32_t)dst & IMG_ADDR_MASK;
            segs[n].src_lo = (uint32_t)lo & IMG_ADDR_MASK;
            segs[n].src_hi = (uint32_t)hi & IMG_ADDR_MASK;
            segs[n].len    = segs[n].src_hi - segs[n].src_lo + 1;
            if (segs[n].dst + segs[n].len > IMG_WORDS) die("段目的地址越界（目的+字数超 12 位空间）");
            n++;
        } else {
            die("未知定义关键字（应为 entry / seg）");
        }
    }
    fclose(f);
    if (n == 0) die("镜像至少需要 1 个段");
    *nseg = n;
}

int main(int argc, char **argv) {
    static char short_opts[] = "o:l:x:";
    static struct option long_opts[] = {
        {"output", required_argument, 0, 'o'},
        {"lst",    required_argument, 0, 'l'},
        {"hex",    required_argument, 0, 'x'},
        {0, 0, 0, 0},
    };

    const char *out_bin = NULL;
    const char *out_lst = NULL;
    const char *out_hex = NULL;
    int c;
    while ((c = getopt_long(argc, argv, short_opts, long_opts, NULL)) != -1) {
        if (c == 'o') out_bin = optarg;
        else if (c == 'l') out_lst = optarg;
        else if (c == 'x') out_hex = optarg;
        else { fprintf(stderr, "Usage: %s <def.seg> <input.103> -o <image.bin> [--lst <image.lst>] [--hex <image.hex>]\n", argv[0]);
               exit(EXIT_FAILURE); }
    }
    if (optind + 1 >= argc || !out_bin) {
        fprintf(stderr, "Usage: %s <def.seg> <input.103> -o <image.bin> [--lst <image.lst>]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    const char *def_path = argv[optind];
    const char *src_path = argv[optind + 1];

    seg_def_t segs[IMG_MAX_SEG];
    int nseg = 0;
    uint32_t entry = 0;
    parse_def(def_path, segs, &nseg, &entry);
    if (entry >= IMG_WORDS) die("入口 PC 越界");

    load_103(src_path);

    /* 校验每段区间内 .103 字齐全（连续区间不得缺字）。 */
    for (int i = 0; i < nseg; i++) {
        for (uint32_t a = segs[i].src_lo; a <= segs[i].src_hi; a++) {
            if (!dict_present[a]) {
                char b[96];
                snprintf(b, sizeof b, "段 %d 缺地址 %04o 的字", i, a);
                die(b);
            }
        }
    }

    /* 计算镜像体字数与各段 body 内字索引。 */
    uint32_t hdr_words = 2;
    uint32_t body_words = 0;
    for (int i = 0; i < nseg; i++) {
        body_words += segs[i].len;
    }

    /* 组装 32 位字数组：头 2 + 段表 2N + body + 校验尾 1。 */
    uint32_t total_words = hdr_words + 2u * (uint32_t)nseg + body_words + 1u;
    uint32_t *words = calloc(total_words, sizeof(uint32_t));
    if (!words) die("内存分配失败");

    words[0] = IMG_MAGIC;
    words[1] = img_pack_hdr1((uint32_t)nseg, entry,
                              (IMG_VERSION << 3) | IMG_ALGO_SUM);

    uint32_t body_word_off = 0;
    uint32_t body_idx = hdr_words + 2u * (uint32_t)nseg;
    for (int i = 0; i < nseg; i++) {
        uint32_t a = img_pack_seg_a(segs[i].dst, body_word_off, segs[i].type, 0);
        uint32_t b = img_pack_seg_b(segs[i].len);
        words[hdr_words + 2u * i]     = a;
        words[hdr_words + 2u * i + 1] = b;

        for (uint32_t k = 0; k < segs[i].len; k++) {
            words[body_idx + k] = dict_mem[segs[i].src_lo + k];
        }
        body_idx      += segs[i].len;
        body_word_off += segs[i].len;
    }

    /* 整体累加和（覆盖头 + 段表 + 镜像体，不含校验尾）。 */
    uint32_t checksum = 0;
    for (uint32_t w = 0; w < total_words - 1u; w++) {
        checksum = (checksum + img_sum_word(words[w])) & IMG_VALUE_MASK;
    }
    words[total_words - 1u] = checksum;

    /* 写二进制（小端）。 */
    FILE *out = fopen(out_bin, "wb");
    if (!out) { char b[256]; snprintf(b, sizeof b, "打不开输出 %s", out_bin); die(b); }
    uint8_t buf[4];
    for (uint32_t w = 0; w < total_words; w++) {
        img_put_le32(buf, words[w]);
        if (fwrite(buf, 1, 4, out) != 4) die("写镜像文件失败");
    }
    fclose(out);

    /* 派生 .lst 十六进制文本清单（分区注释，供 $readmemh / diff）。 */
    if (out_lst) {
        FILE *lst = fopen(out_lst, "w");
        if (!lst) { char b[256]; snprintf(b, sizeof b, "打不开清单 %s", out_lst); die(b); }
        fprintf(lst, "// P0-2 image list: %s\n", src_path);
        fprintf(lst, "// header: magic / hdr1\n");
        fprintf(lst, "%08x\n%08x\n", words[0], words[1]);
        fprintf(lst, "// segment table: A B per segment\n");
        for (int i = 0; i < nseg; i++) {
            fprintf(lst, "%08x\n%08x\n",
                    words[hdr_words + 2u * i], words[hdr_words + 2u * i + 1]);
        }
        fprintf(lst, "// image body\n");
        for (uint32_t k = 0; k < body_words; k++) {
            fprintf(lst, "%08x\n", words[hdr_words + 2u * (uint32_t)nseg + k]);
        }
        fprintf(lst, "// checksum\n%08x\n", words[total_words - 1u]);
        fclose(lst);
    }

    /* 纯十六进制文本镜像（每行一个 32 位字，无注释），供 RTL $readmemh。 */
    if (out_hex) {
        FILE *hex = fopen(out_hex, "w");
        if (!hex) { char b[256]; snprintf(b, sizeof b, "打不开清单 %s", out_hex); die(b); }
        for (uint32_t w = 0; w < total_words; w++) {
            fprintf(hex, "%08x\n", words[w]);
        }
        fclose(hex);
    }

    free(words);
    fprintf(stderr, "image-pack: %d 段, %u 字, 入口 %04o, 校验和 %08x -> %s\n",
            nseg, body_words, entry, checksum, out_bin);
    exit(EXIT_SUCCESS);
}
