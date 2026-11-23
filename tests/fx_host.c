/* host harness: compiles an effect's source as is, drives set()/run() in 256-frame blocks */
#define __attribute__(x)
#include FXSRC
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
/* usage: h  in.raw(int16 stereo)  out.raw  v0 v1 v2 v3 */
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    long frames = sz / 4; int16_t *in = malloc(sz); fread(in, 1, sz, f); fclose(f);
    hb_module.init();
    for (uint32_t k = 0; k < hb_module.n_params; k++) {
        int32_t v = k + 3 < (uint32_t)argc ? atoi(argv[k + 3]) : hb_module.params[k].def;
        if (v < hb_module.params[k].min || v > hb_module.params[k].max) { fprintf(stderr, "param %u out of range\n", k); return 2; }
        hb_module.set(k, v);
    }
    int32_t buf[512]; int16_t *out = malloc(sz);
    for (long i = 0; i < frames; i += 256) {
        long n = frames - i < 256 ? frames - i : 256;
        for (long j = 0; j < 2 * n; j++) buf[j] = in[2 * i + j];
        hb_module.run(buf, n);
        for (long j = 0; j < 2 * n; j++) {
            if (buf[j] > 32767 || buf[j] < -32767) { fprintf(stderr, "OUT OF RANGE %d\n", buf[j]); return 3; }
            out[2 * i + j] = buf[j];
        }
    }
    f = fopen(argv[2], "wb"); fwrite(out, 1, sz, f); fclose(f);
    return 0;
}
