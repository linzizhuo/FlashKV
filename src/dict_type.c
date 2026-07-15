#include"dict_type.h"
#include "sds.h"
#include "object.h"
#include "io.h"

struct dictType dictTypeSds = {
    .hash = sdsHash,
    .keyCompare = sdsCompare,
    .keyFree = sdsfree,
    .valFree = valObjFree,
    .valGet = dictValGetPtr,

    /* 持久化 */
    .keyWrite = (int (*)(Io *, const void *))sdsWrite,
    // .keyDeserialize = (void *(*)(const void *))sdsDeserialize,
    .valWrite = (int (*)(Io *, const void *))valObjWrite,
    // .valDeserialize = (void *(*)(int, const void *))valObjDeserialize,
    .keyRead = (int (*)(Io *, void **))sdsRead,
    .valRead = (int (*)(Io *, int, void **))valObjRead,

};
struct dictType dictTTL = {
    .hash = sdsHash,
    .keyCompare = sdsCompare,
    .keyFree = sdsfree,
    .valFree = NULL,
    .valGet = dictValGetRef,

    // /* 持久化 */
    // .keyWrite = (int (*)(Io *, const void *))sdsWrite,
    // .keyDeserialize = (void *(*)(const void *))sdsDeserialize,
    /* valWrite/valDeserialize 暂不设 — TTL 的 val 是 timestamp，格式不同 */
};