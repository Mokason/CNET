/* detect_cli: probe any model file's structure before running it.
 *
 *   make detect FILE=Models/foo.gguf
 *   bin/detect_cli <path> [<path>...]
 *
 * Prints the structural report (format, family, hparams, runnable verdict)
 * without loading any tensor data. Exit code 0 if every file was probed,
 * 1 on IO/argument errors.
 */

#include <stdio.h>
#include "../include/cce/cce_detect.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: detect_cli <model-file> [<model-file>...]\n");
        return 1;
    }
    int rc_all = 0;
    for (int i = 1; i < argc; i++) {
        cce_model_info info;
        cce_result rc = cce_detect_file(argv[i], &info);
        if (rc != CCE_OK) {
            fprintf(stderr, "error: cannot probe %s (rc=%d)\n", argv[i], (int)rc);
            rc_all = 1;
            continue;
        }
        cce_detect_print(&info, argv[i]);
        if (i + 1 < argc) printf("\n");
    }
    return rc_all;
}
