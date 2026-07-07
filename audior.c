/*
 * audior.c - Command-line entry point.
 *
 * Parses arguments, prints help, and dispatches to either device listing
 * or a recording session.  All audio work lives in the supporting modules
 * (device / capture / writer / recorder).
 *
 * Build: build.bat   (MSVC + Windows SDK 10+)
 */
#include "common.h"
#include "device.h"
#include "recorder.h"

#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Help                                                                */
/* ------------------------------------------------------------------ */

static void PrintUsage(void)
{
    puts(
        "\n"
        "AUDIOR  -  Windows Command-Line Audio Recorder\n"
        "Records the microphone (and optionally system audio) to WAV or MP3.\n"
        "Built on WASAPI + Media Foundation.  Windows 10/11.  No external libraries.\n"
        "\n"
        "USAGE\n"
        "  audior [options]\n"
        "\n"
        "RECORDING\n"
        "  Recording runs in the foreground and continues until you press [Enter],\n"
        "  or until another instance is run with --stop.\n"
        "  Audio is streamed to disk continuously while recording.\n"
        "\n"
        "OPTIONS\n"
        "  -o, --output <file>        Output file path. Required to record.\n"
        "                             The extension is adjusted to match the format\n"
        "                             (e.g. 'meeting' becomes 'meeting.mp3').\n"
        "\n"
        "  -f, --format <wav|mp3>     Output format. Default: mp3.\n"
        "                             mp3 - MPEG Layer-3, 128 kbps (Media Foundation)\n"
        "                             wav - uncompressed PCM, 48 kHz/16-bit/stereo\n"
        "\n"
        "  -a, --amplify <0-300>      Amplify the microphone stream. Default: 0.\n"
        "                             0   = no amplification (unity gain)\n"
        "                             100 = +100% (2x), 300 = +300% (4x)\n"
        "                             Applies to the microphone only; system audio\n"
        "                             loopback is always captured at unity gain.\n"
        "\n"
        "  -i, --include-output       Also capture system audio output (loopback).\n"
        "                             By default only the microphone is recorded.\n"
        "                             You will be asked whether to merge the two\n"
        "                             streams or keep separate files (unless --merge\n"
        "                             or --separate is given).\n"
        "\n"
        "      --merge [percent]      Mix microphone + system audio into one file.\n"
        "                             Optional percent (0-100) is the microphone's\n"
        "                             share of the mix; the system gets the rest.\n"
        "                             Default: 65 (i.e. 65% mic / 35% system).\n"
        "      --separate             Write microphone and system audio to two files\n"
        "                             ('<name>_mic' and '<name>_system').\n"
        "\n"
        "  -m, --mic <id|index|name>  Microphone to record from. Default: system\n"
        "                             default capture device. Accepts a device ID,\n"
        "                             a list index, or part of the device name.\n"
        "\n"
        "  -d, --output-device <id|index|name>\n"
        "                             Playback device to capture for loopback.\n"
        "                             Default: system default render device.\n"
        "\n"
        "  -l, --list-devices         List all capture and playback devices, then\n"
        "                             exit. The system default is marked [DEFAULT].\n"
        "\n"
        "  -s, --stop                 Stop a recording started by another instance\n"
        "                             (signals it to finalise its file and exit).\n"
        "\n"
        "  -h, --help                 Show this help and exit.\n"
        "\n"
        "EXAMPLES\n"
        "  audior --list-devices\n"
        "  audior --output meeting.mp3\n"
        "  audior --output talk.wav --format wav\n"
        "  audior --output session.mp3 --include-output --merge\n"
        "  audior --output lecture.mp3 --amplify 50\n"
        "  audior --output podcast.mp3 -i --merge 70   (70% mic / 30% system)\n"
        "  audior -o call.wav -f wav -i --separate --mic 1\n"
        "  audior --stop                 (from another window, ends the recording)\n"
    );
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

typedef enum { CMD_HELP, CMD_LIST, CMD_RECORD, CMD_STOP } Command;

/* Replace / append the extension matching the chosen format.
 *
 * Why force it: the MP3 file sink picks the container from the URL extension,
 * so a mismatched name (e.g. 'clip.wav' with --format mp3) would confuse the
 * encoder or mislead the user.  Enforcing the extension makes the on-disk name
 * always reflect the actual format.  The dot-before-separator check avoids
 * mistaking a '.' in a directory name for a file extension. */
static void EnforceExtension(wchar_t *path, AudioFormat format)
{
    const wchar_t *ext = (format == FORMAT_MP3) ? L".mp3" : L".wav";
    wchar_t       *dot = wcsrchr(path, L'.');

    if (dot && !wcschr(dot, L'\\') && !wcschr(dot, L'/')) {
        if (_wcsicmp(dot, ext) == 0)
            return;                       /* already correct */
        *dot = L'\0';                     /* strip existing extension */
    }
    wcscat_s(path, MAX_PATH, ext);
}

static void CopyArgW(const char *arg, wchar_t *dst, int cch)
{
    MultiByteToWideChar(CP_ACP, 0, arg, -1, dst, cch);
}

/* True only for a non-empty run of ASCII digits.  Used to decide whether the
 * token after --merge is its optional percentage or the next flag, so we never
 * mistake e.g. "--amplify" (atoi would read 0) for a number. */
static int IsAllDigitsA(const char *s)
{
    if (!s || !*s)
        return 0;
    for (; *s; ++s)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

/* Parse argv into (command, config).  Returns 0 on success, -1 on error. */
static int ParseArgs(int argc, char **argv, Command *cmd, RecordConfig *cfg)
{
    int i;

    memset(cfg, 0, sizeof(*cfg));
    cfg->format          = FORMAT_MP3;  /* default format */
    cfg->mixMode         = MIX_PROMPT;  /* ask if loopback enabled w/o choice */
    cfg->amplifyPercent  = 0;           /* no microphone amplification */
    cfg->mergeMicPercent = DEFAULT_MERGE_MIC_PERCENT;  /* 65/35 mic/system */
    *cmd = CMD_RECORD;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            *cmd = CMD_HELP;
            return 0;
        } else if (!strcmp(a, "-l") || !strcmp(a, "--list-devices")) {
            *cmd = CMD_LIST;
        } else if (!strcmp(a, "-s") || !strcmp(a, "--stop")) {
            *cmd = CMD_STOP;
        } else if (!strcmp(a, "-i") || !strcmp(a, "--include-output")) {
            cfg->includeOutput = TRUE;
        } else if (!strcmp(a, "--merge")) {
            cfg->mixMode = MIX_MERGE;
            /* Optional next token: the microphone's percentage of the mix.
             * Only consumed when it is all digits, so a following flag or the
             * end of the arguments leaves the default (65) in place. */
            if (i + 1 < argc && IsAllDigitsA(argv[i + 1])) {
                int v = atoi(argv[++i]);
                if (v < 0)   { fprintf(stderr, "Warning: --merge percent clamped to 0.\n");   v = 0; }
                if (v > 100) { fprintf(stderr, "Warning: --merge percent clamped to 100.\n"); v = 100; }
                cfg->mergeMicPercent = v;
            }
        } else if (!strcmp(a, "--separate")) {
            cfg->mixMode = MIX_SEPARATE;
        } else if ((!strcmp(a, "-o") || !strcmp(a, "--output")) && i + 1 < argc) {
            CopyArgW(argv[++i], cfg->outputPath, MAX_PATH);
        } else if ((!strcmp(a, "-f") || !strcmp(a, "--format")) && i + 1 < argc) {
            const char *v = argv[++i];
            if (!_stricmp(v, "wav"))      cfg->format = FORMAT_WAV;
            else if (!_stricmp(v, "mp3")) cfg->format = FORMAT_MP3;
            else {
                fprintf(stderr, "Error: unknown format '%s' (use wav or mp3).\n", v);
                return -1;
            }
        } else if ((!strcmp(a, "-m") || !strcmp(a, "--mic")) && i + 1 < argc) {
            CopyArgW(argv[++i], cfg->micSelector, 256);
        } else if ((!strcmp(a, "-d") || !strcmp(a, "--output-device")) && i + 1 < argc) {
            CopyArgW(argv[++i], cfg->renderSelector, 256);
        } else if ((!strcmp(a, "-a") || !strcmp(a, "--amplify")) && i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v < 0)                    { fprintf(stderr, "Warning: --amplify clamped to 0.\n");   v = 0; }
            if (v > MAX_AMPLIFY_PERCENT)  { fprintf(stderr, "Warning: --amplify clamped to 300.\n"); v = MAX_AMPLIFY_PERCENT; }
            cfg->amplifyPercent = v;
        } else {
            fprintf(stderr, "Error: unknown or incomplete argument '%s'.\n"
                            "Run 'audior --help' for usage.\n", a);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    Command      cmd;
    RecordConfig cfg;

    if (argc < 2) {
        PrintUsage();
        return 0;
    }

    if (ParseArgs(argc, argv, &cmd, &cfg) != 0)
        return 1;

    switch (cmd) {
    case CMD_HELP:
        PrintUsage();
        return 0;

    case CMD_LIST: {
        /* Device enumeration is COM; the recording path initialises COM inside
         * Recorder_Run, but the list path must do it here itself. */
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        int     ok;
        if (FAILED(hr)) {
            fprintf(stderr, "CoInitializeEx failed (hr=0x%08lX)\n", (unsigned long)hr);
            return 1;
        }
        ok = SUCCEEDED(Device_ListAll());
        CoUninitialize();
        return ok ? 0 : 1;
    }

    case CMD_STOP: {
        HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, STOP_EVENT_NAME);
        if (!h) {
            fprintf(stderr, "No active recording to stop.\n");
            return 1;
        }
        SetEvent(h);
        CloseHandle(h);
        printf("Stop signal sent.\n");
        return 0;
    }

    case CMD_RECORD:
        if (cfg.outputPath[0] == L'\0') {
            fprintf(stderr, "Error: --output <file> is required.\n"
                            "Run 'audior --help' for usage.\n");
            return 1;
        }
        EnforceExtension(cfg.outputPath, cfg.format);
        return Recorder_Run(&cfg);
    }

    return 1;
}
