#pragma once

// 自研断言：Release 下 assert 会被 NDEBUG 裁掉，CHECK 始终生效

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

// 带附言版本（msg 通常传 err.c_str()，把解析器/校验器的错误描述打出来）
#define CHECK_MSG(cond, msg)                                                               \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "CHECK failed %s:%d: %s\n  msg: %s\n", __FILE__, __LINE__,     \
                    #cond, (msg) ? (msg) : "");                                            \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)
