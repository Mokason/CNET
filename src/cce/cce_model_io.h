#ifndef CCE_MODEL_IO_H
#define CCE_MODEL_IO_H

#include "../../include/cce/cce_defs.h"
#include "../../include/cce/cce_model.h"

cce_result cce_model_io_save(cce_model* model, const char* path);
cce_result cce_model_io_load(cce_model* model, const char* path);

#endif
