#include"dict_type.h"

#include "sds.h"
#include "val_obj.h"

const struct dictType dictTypeSds = {
    .hash = sdsHash,
    .keyCompare = sdsCompare,
    .keyFree = sdsfree,
    .valFree = valObjFree
};