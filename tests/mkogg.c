/*
 * mkogg: a test sound for make test-love, built and run on the host: one
 * second of a 440 Hz tone, stereo, 44.1 kHz, in Ogg Vorbis (libogg and
 * libvorbis, from the sources build.sh fetched). The test game decodes it
 * on PeOS and checks its length and rate.
 *
 *   mkogg out.ogg
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vorbis/vorbisenc.h>

static void write_page(FILE *f, ogg_page *og) {
    fwrite(og->header, 1, (size_t)og->header_len, f);
    fwrite(og->body, 1, (size_t)og->body_len, f);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: mkogg out.ogg\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "wb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    const int rate = 44100, frames = 44100;
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, 2, rate, 0.3f) != 0) return 1;
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "TITLE", "PeOS test tone");
    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_state os;
    ogg_stream_init(&os, 1);
    ogg_packet hdr, hdr_comm, hdr_code;
    vorbis_analysis_headerout(&vd, &vc, &hdr, &hdr_comm, &hdr_code);
    ogg_stream_packetin(&os, &hdr);
    ogg_stream_packetin(&os, &hdr_comm);
    ogg_stream_packetin(&os, &hdr_code);
    ogg_page og;
    while (ogg_stream_flush(&os, &og)) write_page(f, &og);

    int done = 0, pos = 0, eos = 0;
    while (!eos) {
        if (!done) {
            int n = frames - pos > 1024 ? 1024 : frames - pos;
            if (n > 0) {
                float **buf = vorbis_analysis_buffer(&vd, n);
                for (int i = 0; i < n; i++) {
                    float s = 0.4f * sinf(2.0f * 3.14159265f * 440.0f * (float)(pos + i) / (float)rate);
                    buf[0][i] = s;
                    buf[1][i] = s;
                }
                vorbis_analysis_wrote(&vd, n);
                pos += n;
            } else {
                vorbis_analysis_wrote(&vd, 0);
                done = 1;
            }
        }
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);
            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                while (ogg_stream_pageout(&os, &og)) {
                    write_page(f, &og);
                    if (ogg_page_eos(&og)) eos = 1;
                }
            }
        }
    }
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    fclose(f);
    return 0;
}
