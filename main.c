#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "wavpack.h"
#include "utils.h"
#include "win32_unicode_support.h"

#define WAVE_FORMAT_PCM         0x0001
#define WAVE_FORMAT_IEEE_FLOAT  0x0003
#define WAVE_FORMAT_EXTENSIBLE  0xfffe

int debug_logging_mode = 0;

static void set_console_utf8(void)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

typedef struct {
    FILE *file;
    uint32_t bytes_written;
    int error;
} WvFile;

static int write_block(void *id, void *data, int32_t length)
{
    WvFile *wf = (WvFile *)id;
    uint32_t bcount;
    (void)bcount;

    if (wf->error)
        return FALSE;

    if (wf && wf->file && data && length) {
        if (!DoWriteFile(wf->file, data, length, &bcount)) {
            wf->error = 1;
            return FALSE;
        }
        wf->bytes_written += length;
    }
    return TRUE;
}

static void error_line(char *error, ...)
{
    va_list args;
    va_start(args, error);
    vfprintf(stderr, error, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static char *change_extension(const char *filename, const char *new_ext)
{
    char *result = malloc(strlen(filename) + 10);
    if (!result) return NULL;

    strcpy(result, filename);
    char *dot = strrchr(result, '.');
    if (dot)
        *dot = '\0';
    strcat(result, new_ext);
    return result;
}

int encode_wav_to_wv(const char *wav_filename, const char *wv_filename)
{
    FILE *infile = NULL;
    WvFile wv_file = {0};
    WavpackContext *wpc = NULL;
    WavpackConfig config = {0};
    int result = -1;

    infile = fopen_utf8(wav_filename, "rb");
    if (!infile) {
        fprintf(stderr, "can't open input file: %s\n", wav_filename);
        return -1;
    }

    wv_file.file = fopen_utf8(wv_filename, "w+b");
    if (!wv_file.file) {
        fprintf(stderr, "can't create output file: %s\n", wv_filename);
        fclose(infile);
        return -1;
    }

    wpc = WavpackOpenFileOutput(write_block, &wv_file, NULL);
    if (!wpc) {
        fprintf(stderr, "can't create WavPack context\n");
        goto cleanup;
    }

    char fourcc[5] = {0};
    uint32_t bcount;
    if (!DoReadFile(infile, fourcc, 4, &bcount) || bcount != 4) {
        fprintf(stderr, "can't read RIFF header\n");
        goto cleanup;
    }

    if (strncmp(fourcc, "RIFF", 4) && strncmp(fourcc, "RF64", 4)) {
        fprintf(stderr, "not a valid WAV file\n");
        goto cleanup;
    }

    RiffChunkHeader riff_header;
    memcpy(riff_header.ckID, fourcc, 4);
    riff_header.ckSize = 0;
    memcpy(riff_header.formType, "WAVE", 4);

    if (!DoReadFile(infile, riff_header.formType, 8, &bcount) || bcount != 8) {
        fprintf(stderr, "can't read RIFF header\n");
        goto cleanup;
    }

    if (!WavpackAddWrapper(wpc, &riff_header, sizeof(RiffChunkHeader))) {
        fprintf(stderr, "%s\n", WavpackGetErrorMessage(wpc));
        goto cleanup;
    }

    int found_fmt = 0, found_data = 0;
    int64_t total_samples = 0;
    int num_channels = 0, sample_rate = 0, bits_per_sample = 0, bytes_per_sample = 0, format = 0;
    uint32_t channel_mask = 0;

    while (!found_data) {
        ChunkHeader chunk_header;
        if (!DoReadFile(infile, &chunk_header, sizeof(ChunkHeader), &bcount) || bcount != sizeof(ChunkHeader)) {
            fprintf(stderr, "can't read chunk header\n");
            goto cleanup;
        }

        WavpackLittleEndianToNative(&chunk_header, "4L");

        if (!strncmp(chunk_header.ckID, "fmt ", 4)) {
            WaveHeader wave_header;
            memset(&wave_header, 0, sizeof(wave_header));

            if (chunk_header.ckSize < 16 || chunk_header.ckSize > sizeof(wave_header)) {
                fprintf(stderr, "invalid fmt chunk size\n");
                goto cleanup;
            }

            if (!DoReadFile(infile, &wave_header, chunk_header.ckSize, &bcount) || bcount != chunk_header.ckSize) {
                fprintf(stderr, "can't read fmt chunk\n");
                goto cleanup;
            }

            WavpackLittleEndianToNative(&wave_header, "SSLLSSSSLS");

            if (!WavpackAddWrapper(wpc, &chunk_header, sizeof(ChunkHeader)) ||
                !WavpackAddWrapper(wpc, &wave_header, chunk_header.ckSize)) {
                fprintf(stderr, "%s\n", WavpackGetErrorMessage(wpc));
                goto cleanup;
            }

            int extensible = wave_header.FormatTag == WAVE_FORMAT_EXTENSIBLE && chunk_header.ckSize >= 18;
            format = (extensible && chunk_header.ckSize >= 8) ? wave_header.SubFormat : wave_header.FormatTag;

            if (format != WAVE_FORMAT_PCM && format != WAVE_FORMAT_IEEE_FLOAT) {
                fprintf(stderr, "unsupported audio format\n");
                goto cleanup;
            }

            if (format == WAVE_FORMAT_IEEE_FLOAT && wave_header.BitsPerSample != 32) {
                fprintf(stderr, "only 32-bit float supported\n");
                goto cleanup;
            }

            num_channels = wave_header.NumChannels;
            sample_rate = wave_header.SampleRate;
            bits_per_sample = (extensible && chunk_header.ckSize >= 2 && wave_header.ValidBitsPerSample) ?
                wave_header.ValidBitsPerSample : wave_header.BitsPerSample;
            bytes_per_sample = wave_header.BlockAlign / wave_header.NumChannels;
            channel_mask = 0;

            if (extensible && chunk_header.ckSize >= 6)
                channel_mask = wave_header.ChannelMask;

            if (!channel_mask && num_channels <= 2)
                channel_mask = 0x5 - num_channels;

            found_fmt = 1;
        }
        else if (!strncmp(chunk_header.ckID, "data", 4)) {
            found_data = 1;

            if (!found_fmt) {
                fprintf(stderr, "fmt chunk not found before data\n");
                goto cleanup;
            }

            int64_t data_size;
            if (chunk_header.ckSize == (uint32_t)-1) {
                int64_t file_size = DoGetFileSize(infile);
                int64_t pos = DoGetFilePosition(infile);
                data_size = file_size - pos;
            } else {
                data_size = chunk_header.ckSize;
            }

            total_samples = data_size / (bytes_per_sample * num_channels);

            if (!WavpackAddWrapper(wpc, &chunk_header, sizeof(ChunkHeader))) {
                fprintf(stderr, "%s\n", WavpackGetErrorMessage(wpc));
                goto cleanup;
            }
        }
        else {
            if (!WavpackAddWrapper(wpc, &chunk_header, sizeof(ChunkHeader))) {
                fprintf(stderr, "%s\n", WavpackGetErrorMessage(wpc));
                goto cleanup;
            }

            if (chunk_header.ckSize > 0) {
                uint8_t *buffer = (uint8_t *)malloc(chunk_header.ckSize);
                if (buffer) {
                    DoReadFile(infile, buffer, chunk_header.ckSize, &bcount);
                    WavpackAddWrapper(wpc, buffer, chunk_header.ckSize);
                    free(buffer);
                } else {
                    DoSetFilePositionRelative(infile, chunk_header.ckSize, SEEK_CUR);
                }
            }

            if (chunk_header.ckSize & 1)
                DoSetFilePositionRelative(infile, 1, SEEK_CUR);
        }
    }

    config.bits_per_sample = bits_per_sample;
    config.bytes_per_sample = bytes_per_sample;
    config.num_channels = num_channels;
    config.sample_rate = sample_rate;
    config.channel_mask = channel_mask;
    config.qmode = 0;

    if (!WavpackSetConfiguration64(wpc, &config, total_samples, NULL)) {
        fprintf(stderr, "%s\n", WavpackGetErrorMessage(wpc));
        goto cleanup;
    }

    if (!WavpackPackInit(wpc)) {
        fprintf(stderr, "%s\n", WavpackGetErrorMessage(wpc));
        goto cleanup;
    }

    int32_t *sample_buffer = (int32_t *)malloc((size_t)4096 * num_channels * sizeof(int32_t));
    if (!sample_buffer) {
        fprintf(stderr, "memory allocation failed\n");
        goto cleanup;
    }

    while (1) {
        uint32_t samples_to_read = 4096;
        uint32_t samples_read = 0;

        if (found_data && total_samples > 0) {
            int64_t file_pos = DoGetFilePosition(infile);
            int64_t data_start = file_pos - sizeof(ChunkHeader);
            if (num_channels * bytes_per_sample > 0) {
                int64_t current_sample = (file_pos - data_start - (int64_t)sizeof(ChunkHeader)) / ((int64_t)num_channels * bytes_per_sample);
                if (current_sample >= total_samples)
                    break;
                if ((uint64_t)current_sample + samples_to_read > (uint64_t)total_samples)
                    samples_to_read = (uint32_t)(total_samples - current_sample);
            }
        }

        if (bytes_per_sample == 1) {
            uint8_t *data8 = (uint8_t *)malloc((size_t)samples_to_read * num_channels);
            if (!data8) break;
            DoReadFile(infile, data8, (size_t)samples_to_read * num_channels, &bcount);
            samples_read = (uint32_t)(bcount / num_channels);
            for (uint32_t i = 0; i < samples_read * num_channels; i++)
                sample_buffer[i] = (int32_t)data8[i] - 128;
            free(data8);
        } else if (bytes_per_sample == 2) {
            int16_t *data16 = (int16_t *)malloc((size_t)samples_to_read * num_channels * sizeof(int16_t));
            if (!data16) break;
            DoReadFile(infile, data16, (size_t)samples_to_read * num_channels * sizeof(int16_t), &bcount);
            samples_read = (uint32_t)(bcount / (num_channels * (uint32_t)sizeof(int16_t)));
            for (uint32_t i = 0; i < samples_read * num_channels; i++)
                WavpackLittleEndianToNative(&data16[i], "s");
            for (uint32_t i = 0; i < samples_read * num_channels; i++)
                sample_buffer[i] = data16[i];
            free(data16);
        } else if (bytes_per_sample == 3) {
            uint8_t *data8 = (uint8_t *)malloc((size_t)samples_to_read * num_channels * 3);
            if (!data8) break;
            DoReadFile(infile, data8, (size_t)samples_to_read * num_channels * 3, &bcount);
            samples_read = (uint32_t)(bcount / ((uint32_t)num_channels * 3));
            for (uint32_t i = 0; i < samples_read * num_channels; i++) {
                int32_t s = data8[i * 3] | (data8[i * 3 + 1] << 8) | ((int8_t)data8[i * 3 + 2] << 16);
                sample_buffer[i] = s;
            }
            free(data8);
        } else if (bytes_per_sample == 4) {
            if (bits_per_sample == 32 && format == WAVE_FORMAT_IEEE_FLOAT) {
                float *data32 = (float *)malloc((size_t)samples_to_read * num_channels * sizeof(float));
                if (!data32) break;
                DoReadFile(infile, data32, (size_t)samples_to_read * num_channels * sizeof(float), &bcount);
                samples_read = (uint32_t)(bcount / ((uint32_t)num_channels * (uint32_t)sizeof(float)));
                for (uint32_t i = 0; i < samples_read * num_channels; i++)
                    WavpackLittleEndianToNative(&data32[i], "f");
                for (uint32_t i = 0; i < samples_read * num_channels; i++)
                    sample_buffer[i] = ((int32_t*)data32)[i];
                free(data32);
            } else {
                int32_t *data32 = (int32_t *)malloc((size_t)samples_to_read * num_channels * sizeof(int32_t));
                if (!data32) break;
                DoReadFile(infile, data32, (size_t)samples_to_read * num_channels * sizeof(int32_t), &bcount);
                samples_read = (uint32_t)(bcount / ((uint32_t)num_channels * (uint32_t)sizeof(int32_t)));
                for (uint32_t i = 0; i < samples_read * num_channels; i++)
                    WavpackLittleEndianToNative(&data32[i], "l");
                for (uint32_t i = 0; i < samples_read * num_channels; i++)
                    sample_buffer[i] = data32[i];
                free(data32);
            }
        }

        if (samples_read == 0)
            break;

        if (!WavpackPackSamples(wpc, sample_buffer, samples_read)) {
            fprintf(stderr, "%s\n", WavpackGetErrorMessage(wpc));
            free(sample_buffer);
            goto cleanup;
        }
    }

    free(sample_buffer);

    if (!WavpackFlushSamples(wpc)) {
        fprintf(stderr, "%s\n", WavpackGetErrorMessage(wpc));
        goto cleanup;
    }

    result = 0;
    printf("encoded %s -> %s\n", wav_filename, wv_filename);

cleanup:
    if (wpc) WavpackCloseFile(wpc);
    if (infile) DoCloseHandle(infile);
    if (wv_file.file) DoCloseHandle(wv_file.file);
    if (result != 0 && wv_filename)
        DoDeleteFile((char*)wv_filename);

    return result;
}

int decode_wv_to_wav(const char *wv_filename, const char *wav_filename)
{
    char error[256];
    WavpackContext *wpc = NULL;
    FILE *outfile = NULL;
    int result = -1;

    wpc = WavpackOpenFileInput(wv_filename, error, OPEN_WVC | OPEN_WRAPPER, 0);
    if (!wpc) {
        fprintf(stderr, "can't open file: %s\n%s\n", wv_filename, error);
        return -1;
    }

    outfile = fopen_utf8(wav_filename, "wb");
    if (!outfile) {
        fprintf(stderr, "can't create file: %s\n", wav_filename);
        goto cleanup;
    }

    int num_channels = WavpackGetNumChannels(wpc);
    int sample_rate = WavpackGetSampleRate(wpc);
    int bits_per_sample = WavpackGetBitsPerSample(wpc);
    int bytes_per_sample = WavpackGetBytesPerSample(wpc);
    int64_t total_samples = WavpackGetNumSamples64(wpc);
    int format_tag = (WavpackGetFloatNormExp(wpc) == 127) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;

    RiffChunkHeader riffhdr;
    ChunkHeader fmthdr, datahdr;
    WaveHeader wavhdr;
    uint32_t bcount;

    memset(&riffhdr, 0, sizeof(riffhdr));
    memset(&fmthdr, 0, sizeof(fmthdr));
    memset(&datahdr, 0, sizeof(datahdr));
    memset(&wavhdr, 0, sizeof(wavhdr));

    memcpy(riffhdr.ckID, "RIFF", 4);
    memcpy(riffhdr.formType, "WAVE", 4);

    memcpy(fmthdr.ckID, "fmt ", 4);
    fmthdr.ckSize = 16;

    wavhdr.FormatTag = (uint16_t)format_tag;
    wavhdr.NumChannels = (uint16_t)num_channels;
    wavhdr.SampleRate = (uint32_t)sample_rate;
    wavhdr.BytesPerSecond = (uint32_t)(sample_rate * num_channels * bytes_per_sample);
    wavhdr.BlockAlign = (uint16_t)(num_channels * bytes_per_sample);
    wavhdr.BitsPerSample = (uint16_t)bits_per_sample;

    memcpy(datahdr.ckID, "data", 4);
    datahdr.ckSize = (uint32_t)(total_samples * num_channels * bytes_per_sample);

    riffhdr.ckSize = (uint32_t)(sizeof(fmthdr) + fmthdr.ckSize + sizeof(datahdr) + datahdr.ckSize);

    WavpackNativeToLittleEndian(&riffhdr, "4L");
    WavpackNativeToLittleEndian(&fmthdr, "4L");
    WavpackNativeToLittleEndian(&wavhdr, "SSLLSSSSLS");
    WavpackNativeToLittleEndian(&datahdr, "4L");

    DoWriteFile(outfile, &riffhdr, sizeof(riffhdr), &bcount);
    DoWriteFile(outfile, &fmthdr, sizeof(fmthdr), &bcount);
    DoWriteFile(outfile, &wavhdr, fmthdr.ckSize, &bcount);
    DoWriteFile(outfile, &datahdr, sizeof(datahdr), &bcount);

    int32_t *sample_buffer = (int32_t *)malloc((size_t)4096 * num_channels * sizeof(int32_t));
    if (!sample_buffer) {
        fprintf(stderr, "memory allocation failed\n");
        goto cleanup;
    }

    while (1) {
        uint32_t samples_unpacked = WavpackUnpackSamples(wpc, sample_buffer, 4096);
        if (samples_unpacked == 0)
            break;

        uint32_t bytes_to_write = samples_unpacked * (uint32_t)num_channels * (uint32_t)bytes_per_sample;

        if (bytes_per_sample == 1) {
            uint8_t *data8 = (uint8_t *)malloc(bytes_to_write);
            if (!data8) {
                free(sample_buffer);
                goto cleanup;
            }
            for (uint32_t i = 0; i < samples_unpacked * (uint32_t)num_channels; i++)
                data8[i] = (uint8_t)(sample_buffer[i] + 128);
            DoWriteFile(outfile, data8, bytes_to_write, &bcount);
            free(data8);
        } else if (bytes_per_sample == 2) {
            int16_t *data16 = (int16_t *)malloc(bytes_to_write);
            if (!data16) {
                free(sample_buffer);
                goto cleanup;
            }
            for (uint32_t i = 0; i < samples_unpacked * (uint32_t)num_channels; i++)
                data16[i] = (int16_t)sample_buffer[i];
            for (uint32_t i = 0; i < samples_unpacked * (uint32_t)num_channels; i++)
                WavpackNativeToLittleEndian(&data16[i], "s");
            DoWriteFile(outfile, data16, bytes_to_write, &bcount);
            free(data16);
        } else if (bytes_per_sample == 3) {
            uint8_t *data8 = (uint8_t *)malloc(bytes_to_write);
            if (!data8) {
                free(sample_buffer);
                goto cleanup;
            }
            for (uint32_t i = 0; i < samples_unpacked * (uint32_t)num_channels; i++) {
                int32_t s = sample_buffer[i];
                data8[i * 3] = (uint8_t)(s & 0xff);
                data8[i * 3 + 1] = (uint8_t)((s >> 8) & 0xff);
                data8[i * 3 + 2] = (uint8_t)((s >> 16) & 0xff);
            }
            DoWriteFile(outfile, data8, bytes_to_write, &bcount);
            free(data8);
        } else if (bytes_per_sample == 4) {
            if (WavpackGetFloatNormExp(wpc) == 127) {
                float *data32 = (float *)malloc(bytes_to_write);
                if (!data32) {
                    free(sample_buffer);
                    goto cleanup;
                }
                for (uint32_t i = 0; i < samples_unpacked * (uint32_t)num_channels; i++)
                    data32[i] = ((float*)sample_buffer)[i];
                for (uint32_t i = 0; i < samples_unpacked * (uint32_t)num_channels; i++)
                    WavpackNativeToLittleEndian(&data32[i], "f");
                DoWriteFile(outfile, data32, bytes_to_write, &bcount);
                free(data32);
            } else {
                int32_t *data32 = (int32_t *)malloc(bytes_to_write);
                if (!data32) {
                    free(sample_buffer);
                    goto cleanup;
                }
                for (uint32_t i = 0; i < samples_unpacked * (uint32_t)num_channels; i++)
                    data32[i] = sample_buffer[i];
                for (uint32_t i = 0; i < samples_unpacked * (uint32_t)num_channels; i++)
                    WavpackNativeToLittleEndian(&data32[i], "l");
                DoWriteFile(outfile, data32, bytes_to_write, &bcount);
                free(data32);
            }
        }
    }

    free(sample_buffer);
    result = 0;
    printf("decoded %s -> %s\n", wv_filename, wav_filename);

cleanup:
    if (outfile) DoCloseHandle(outfile);
    if (wpc) WavpackCloseFile(wpc);

    return result;
}

int main(int argc, char **argv)
{
    set_console_utf8();
    int argc_utf8;
    char **argv_utf8;
    init_commandline_arguments_utf8(&argc_utf8, &argv_utf8);

    if (argc_utf8 < 3) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  encode: %s -e input.wav\n", argv_utf8[0]);
        fprintf(stderr, "  decode: %s -d input.wv\n", argv_utf8[0]);
        free_commandline_arguments_utf8(&argc_utf8, &argv_utf8);
        return 1;
    }

    int result = 1;
    char *output_filename = NULL;

    if (strcmp(argv_utf8[1], "-e") == 0) {
        output_filename = change_extension(argv_utf8[2], ".wv");
        if (!output_filename) {
            fprintf(stderr, "memory allocation failed\n");
        } else {
            result = encode_wav_to_wv(argv_utf8[2], output_filename);
            free(output_filename);
        }
    } else if (strcmp(argv_utf8[1], "-d") == 0) {
        output_filename = change_extension(argv_utf8[2], ".wav");
        if (!output_filename) {
            fprintf(stderr, "memory allocation failed\n");
        } else {
            result = decode_wv_to_wav(argv_utf8[2], output_filename);
            free(output_filename);
        }
    } else {
        fprintf(stderr, "unknown option: %s (use '-e' or '-d')\n", argv_utf8[1]);
    }

    free_commandline_arguments_utf8(&argc_utf8, &argv_utf8);
    return result;
}