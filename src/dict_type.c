#include"dict_type.h"

#include "sds.h"
#include "val_obj.h"

struct dictType dictTypeSds = {
    .hash = sdsHash,
    .keyCompare = sdsCompare,
    .keyFree = sdsfree,
    .valFree = valObjFree
};

