#include"dict_type.h"
#include "sds.h"
#include "val_obj.h"

struct dictType dictTypeSds = {
    .hash = sdsHash,
    .keyCompare = sdsCompare,
    .keyFree = sdsfree,
    .valFree = valObjFree,
    .valGet = dictValGetPtr,

    /* 序列化 */
    .keySerialize = (size_t (*)(const void *, void **))sdsSerialize,
    .keyDeserialize = (void *(*)(const void *))sdsDeserialize,
    .valSerialize = (size_t (*)(const void *, void **))valObjSerialize,
    .valDeserialize = (void *(*)(int, const void *))valObjDeserialize,
};

struct dictType dictTTL = {
    .hash = sdsHash,
    .keyCompare = sdsCompare,
    .keyFree = sdsfree,
    .valFree = NULL,
    .valGet = dictValGetRef,

    /* 序列化 */
    .keySerialize = (size_t (*)(const void *, void **))sdsSerialize,
    .keyDeserialize = (void *(*)(const void *))sdsDeserialize,
    /* valSerialize/valDeserialize 暂不设 — TTL 的 val 是 timestamp，格式不同 */
};