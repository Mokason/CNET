/* int8_to_trit: take a .int8data from gguf_partial_convert (oracle int8)
 * and produce a much smaller .tritdata using per-column absmean ternary {-1,0,1}
 * packed 5 trits per byte.
 *
 * This gives significant size reduction ( ~1.6 bit/weight + f32 scales per out ).
 * Quality: post-hoc, may lose some vs the int8 version.
 * Usage: ./bin/int8_to_trit in.int8data out.tritdata
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static int8_t ternarize(float x, float gamma) {
    float t = x / gamma;
    if (t > 0.5f) return 1;
    if (t < -0.5f) return -1;
    return 0;
}

static void pack_trits(const int8_t* trits, size_t n, uint8_t* out, size_t* out_bytes) {
    // Simple 5-trit per byte packing (values -1,0,1 mapped to 0,1,2)
    // trit = val + 1  => 0,1,2
    size_t bytes = (n + 4) / 5;
    *out_bytes = bytes;
    memset(out, 0, bytes);
    for (size_t i = 0; i < n; i++) {
        int v = trits[i] + 1; // 0,1,2
        size_t byte_idx = i / 5;
        size_t shift = (i % 5) * 3;  // 3 bits? Wait, for 3^5=243 <256, can pack in ~8 bits but for simplicity use 8-bit per 5?
        // Better standard: use the project's style or byte pack.
        // For simplicity here: store as 2 bits per trit (wasteful) or proper.
        // Use  the 5 trits in one byte using powers.
        // Actually for demo, we'll use a simple byte-per-4-trits or implement proper.
        // Proper 5-trit byte:
        // byte = t0 + t1*3 + t2*9 + t3*27 + t4*81
        if ((i % 5) == 0) {
            // start new
        }
    }
    // Simpler reliable pack for now:  use 1 byte per trit (easy, then compress conceptually)
    // To get real 1.6, implement the base3 pack.
    // Re-implement:
    *out_bytes = 0;
    size_t o = 0;
    for (size_t g = 0; g < n; g += 5) {
        uint8_t b = 0;
        for (int k=0; k<5; k++) {
            if (g+k >= n) break;
            int t = trits[g+k] + 1; // 0 1 2
            b += t * (uint8_t)powf(3, k);
        }
        out[o++] = b;
    }
    *out_bytes = o;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.int8data out.tritdata\n", argv[0]);
        return 1;
    }
    const char* in = argv[1];
    const char* out = argv[2];

    FILE* f = fopen(in, "rb");
    if (!f) { perror("open in"); return 2; }

    uint32_t magic, ver, nb;
    fread(&magic,4,1,f);
    fread(&ver,4,1,f);
    fread(&nb,4,1,f);
    if (magic != 0x3843544e) {
        fprintf(stderr, "bad magic\n");
        fclose(f); return 3;
    }

    FILE* of = fopen(out, "wb");
    uint32_t tmagic = 0x54495254; // TRIT
    uint32_t tver = 1;
    fwrite(&tmagic,4,1,of);
    fwrite(&tver,4,1,of);
    fwrite(&nb,4,1,of);

    size_t total_trit_bytes = 0;

    for (uint32_t i=0; i<nb; i++) {
        uint32_t nlen;
        fread(&nlen,4,1,f);
        char name[256] = {0};
        fread(name,1,nlen,f);
        int32_t idim, odim;
        int8_t hasb;
        fread(&idim,4,1,f);
        fread(&odim,4,1,f);
        fread(&hasb,1,1,f);

        float* scales = malloc(odim * sizeof(float));
        fread(scales, sizeof(float), odim, f);

        size_t csz = (size_t)idim * odim;
        int8_t* codes = malloc(csz);
        fread(codes, 1, csz, f);

        float* bias = NULL;
        if (hasb) {
            bias = malloc(odim * sizeof(float));
            fread(bias, sizeof(float), odim, f);
        }

        // For each output column, compute gamma and ternarize
        int8_t* trits = malloc(csz);
        float* tscales = malloc(odim * sizeof(float));

        for (int o=0; o<odim; o++) {
            float sumabs = 0;
            for (int ii=0; ii<idim; ii++) {
                float w = (float)codes[(size_t)ii * odim + o];
                sumabs += fabsf(w);
            }
            float gamma = sumabs / idim;
            if (gamma < 1e-8f) gamma = 1.0f;
            tscales[o] = gamma;
            for (int ii=0; ii<idim; ii++) {
                float w = (float)codes[(size_t)ii * odim + o];
                trits[(size_t)ii * odim + o] = ternarize(w, gamma);
            }
        }

        // pack trits
        uint8_t* packed = malloc( (csz + 4)/5 + 1 );
        size_t pbytes = 0;
        pack_trits(trits, csz, packed, &pbytes);

        // write to out
        uint32_t nn = nlen;
        fwrite(&nn,4,1,of);
        fwrite(name,1,nlen,of);
        fwrite(&idim,4,1,of);
        fwrite(&odim,4,1,of);
        fwrite(&hasb,1,1,of);
        fwrite(tscales, sizeof(float), odim, of);
        uint32_t ps = (uint32_t)pbytes;
        fwrite(&ps,4,1,of);
        fwrite(packed, 1, pbytes, of);
        if (hasb) {
            fwrite(bias, sizeof(float), odim, of);
        }

        total_trit_bytes += pbytes + odim*4 + (hasb ? odim*4 : 0) + 4/*pbytes field*/ ;

        free(scales);
        free(codes);
        free(trits);
        free(tscales);
        free(packed);
        if (bias) free(bias);
    }

    fclose(f);
    fclose(of);

    fprintf(stderr, "Wrote %s (packed trits)\n", out);
    return 0;
}