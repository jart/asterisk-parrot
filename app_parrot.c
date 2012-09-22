/*
 * phoneparrot - Phone Parrot
 * Version 0.1.1
 *
 * Copyright (c) 2006-2012 Justine Tunney
 *
 * Justine Tunney <jtunney@lobstertech.com>
 *
 * Keep it Open Source Pigs
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License version 2.0 or later.
 */

#define AST_MODULE "app_parrot"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <asterisk.h>
#include <asterisk/file.h>
#include <asterisk/frame.h>
#include <asterisk/options.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/lock.h>
#include <asterisk/app.h>
#include <asterisk/dsp.h>
#ifdef _LIBSOUNDTOUCH4C_
#include <soundtouch4c.h>
#endif
#include <asterisk/stringfields.h>

static char *moddesc = "Phone Parrot";

static char *parrot_app = "Parrot";
static char *parrot_synopsis = "Repeats back to caller what they say like a 5-year-old";
static char *parrot_descrip = ""
    "Parrot(options)\n"
    "\n"
    "Description:\n"
    "  When this application is invoked, it will record to memory what\n"
    "  the caller is saying.  When the caller stops talking, recording\n"
    "  buffer of what s?he just said is played back.\n"
    "\n"
#ifdef _LIBSOUNDTOUCH4C_
    "  You may also chose to add a voice pitch change effect to what\n"
    "  the caller just said with the P() option.  Pitch is specified\n"
    "  in semi-tones.  -5.0 is good for making the voice lower and\n"
    "  5.0 is good for making the voice higher.\n"
    "\n"
#else
    "  If you install SoundTouch and libsoundtouch4c (Also available\n"
    "  at www.lobstertech.com) and then recompile this module, it\n"
    "  will include support for changing the pitch of the caller\'s\n"
    "  voice when echo\'d back.\n"
    "\n"
#endif
    "Options:\n"
    "  T(...) - Silence Threshold, default 256\n"
    "  W(...) - Milliseconds of silence before repeat.  Default 1000\n"
    "  N(...) - Minimum time in milliseconds the caller needs to talk\n"
    "           for voice to be echo'd back.  Default 400\n"
    "  M(...) - Max milliseconds of talk to buffer.  Default 6000\n"
    "  S(...) - If caller rambles for more than length of call buffer,\n"
    "           play a random one of these sound clips instead. List\n"
    "           is delimited by '^'.  Maximum of 16.  These may not be\n"
    "           interrupted.\n"
    "  G(...) - Play greeting clip in response to initial thing caller\n"
    "           says.  In my testing I noticed that people get a little\n"
    "           confused and just keep saying hello if you start them\n"
    "           off in plain old parrot mode.  Here, you can specify a\n"
    "           list of sound clips from which to randomly choose just\n"
    "           like with S() here.\n"
    "  H(...) - Hangup after a certain number of seconds\n"
    "  I(...) - Repeat may be interrupted if caller speaks for '...'\n"
    "           milliseconds.  Default is 400, 0 turns off\n"
#ifdef _LIBSOUNDTOUCH4C_
    "  P(...) - New pitch of voice echo'd back. Default 0.0\n"
#endif
    ;

#define OPT_PARROT_THRESHOLD  (1 << 0)
#define OPT_PARROT_WAIT       (1 << 1)
#define OPT_PARROT_MINTALK    (1 << 2)
#define OPT_PARROT_MAXTALK    (1 << 3)
#define OPT_PARROT_SOUNDCLIPS (1 << 4)
#define OPT_PARROT_GREETCLIPS (1 << 5)
#define OPT_PARROT_HANGUP     (1 << 6)
#define OPT_PARROT_INTERRUPT  (1 << 7)
#define OPT_PARROT_PITCH      (1 << 8)

#define PARROT_MAX_CLIPS 16

enum {
    OPT_ARG_PARROT_THRESHOLD = 0,
    OPT_ARG_PARROT_WAIT,
    OPT_ARG_PARROT_MAXTALK,
    OPT_ARG_PARROT_SOUNDCLIPS,
    OPT_ARG_PARROT_GREETCLIPS,
    OPT_ARG_PARROT_HANGUP,
    OPT_ARG_PARROT_MINTALK,
    OPT_ARG_PARROT_INTERRUPT,
    OPT_ARG_PARROT_PITCH,
    /* note: this entry _MUST_ be the last one in the enum */
    OPT_ARG_PARROT_ARRAY_SIZE
} parrot_option_args;

AST_APP_OPTIONS(parrot_options, {
    AST_APP_OPTION_ARG('T', OPT_PARROT_THRESHOLD, OPT_ARG_PARROT_THRESHOLD),
    AST_APP_OPTION_ARG('W', OPT_PARROT_WAIT, OPT_ARG_PARROT_WAIT),
    AST_APP_OPTION_ARG('M', OPT_PARROT_MAXTALK, OPT_ARG_PARROT_MAXTALK),
    AST_APP_OPTION_ARG('S', OPT_PARROT_SOUNDCLIPS, OPT_ARG_PARROT_SOUNDCLIPS),
    AST_APP_OPTION_ARG('G', OPT_PARROT_GREETCLIPS, OPT_ARG_PARROT_GREETCLIPS),
    AST_APP_OPTION_ARG('H', OPT_PARROT_HANGUP, OPT_ARG_PARROT_HANGUP),
    AST_APP_OPTION_ARG('N', OPT_PARROT_MINTALK, OPT_ARG_PARROT_MINTALK),
    AST_APP_OPTION_ARG('I', OPT_PARROT_INTERRUPT, OPT_ARG_PARROT_INTERRUPT),
    AST_APP_OPTION_ARG('P', OPT_PARROT_PITCH, OPT_ARG_PARROT_PITCH),
});

struct parrot_ops {
    int options;
    int threshold;
    int wait;
    int mintalk;
    int maxtalk;
    char *soundclips[PARROT_MAX_CLIPS];
    int soundclipcnt;
    char *greetclips[PARROT_MAX_CLIPS];
    int greetclipcnt;
    int hangup;
    int interrupt;
    float pitch;
};

struct mybuffer {
    short *buf;
    int    len;
    short *cur;
    short *end;
};

#ifdef _LIBSOUNDTOUCH4C_
static struct soundtouch *soundtouch_create(float newPitch)
{
    struct soundtouch *snd;

    ast_log(LOG_DEBUG, "Creating SoundTouch object...\n");
    snd = SoundTouch_construct();
    if (!snd) {
        ast_log(LOG_WARNING, "Failed to create SoundTouch object\n");
        return NULL;
    }

    SoundTouch_setChannels(snd, 1);
    SoundTouch_setSampleRate(snd, 8000);
    SoundTouch_setPitchSemiTonesFloat(snd, newPitch);
    SoundTouch_setSetting(snd, SETTING_USE_QUICKSEEK, 1);
    SoundTouch_setSetting(snd, SETTING_USE_AA_FILTER, 1);

    return snd;
}

static void soundtouch_free(struct soundtouch *st)
{
    ast_log(LOG_DEBUG, "Freeing SoundTouch object...\n");
    SoundTouch_destruct(st);
}
#endif

#define CONTINUEFREE { ast_frfree(f); continue; }
#define BREAKFREE { ast_frfree(f); break; }
#define SAMPPERFRAME 160

/* -1 on error/hangup, 0 on success */
static int parrot_main(struct ast_channel *chan, const struct parrot_ops *ops)
{
    enum { MODE_SILENCE,
           MODE_RECORD,
           MODE_PENDING } mode = MODE_SILENCE;
    struct ast_frame *f = NULL;
    struct ast_dsp *dsp;
    int hanguptime = 0;
    unsigned long samptotal = 0;
    int msofsilence = 0;
    short data[SAMPPERFRAME]; /* used to temp copy frame data */
    struct mybuffer in, out;
#ifdef _LIBSOUNDTOUCH4C_
    struct soundtouch *st;
#endif
    int greeted = 0;

    if ((ops->options & OPT_PARROT_HANGUP) == OPT_PARROT_HANGUP) {
        hanguptime = time(NULL) + ops->hangup;
    }

    struct ast_format fsl[1] = {{ .id = AST_FORMAT_SLINEAR }};
    if (ast_set_read_format(chan, fsl) < 0 ||
        ast_set_write_format(chan, fsl) < 0) {
        ast_log(LOG_WARNING, "Unable to set channel i/o to slinear mode\n");
        return -1;
    }
    if (!(dsp = ast_dsp_new())) {
        ast_log(LOG_WARNING, "Unable to create DSP object\n");
        return -1;
    }
    ast_dsp_set_threshold(dsp, ops->threshold);

    in.len  = ops->maxtalk * 8/*kHz*/;
    out.len = ops->maxtalk * 8/*kHz*/;

    if (!(in.buf = malloc(in.len * sizeof(short)))) {
        ast_log(LOG_ERROR, "Out of memory!\n");
        ast_dsp_free(dsp);
        return -1;
    }
    if (!(out.buf = malloc(out.len * sizeof(short)))) {
        ast_log(LOG_ERROR, "Out of memory!\n");
        free(in.buf);
        ast_dsp_free(dsp);
        return -1;
    }

#ifdef _LIBSOUNDTOUCH4C_
    if (!(st = soundtouch_create(ops->pitch))) {
        free(in.buf);
        free(out.buf);
        ast_dsp_free(dsp);
        return -1;
    }
#endif

    in.end = in.cur = in.buf;
    out.end = out.cur = out.buf;

    while (ast_waitfor(chan, -1) > -1) {
        if (ast_check_hangup(chan))
            break;
        if (hanguptime && time(NULL) >= hanguptime)
            break;
        if (!(f = ast_read(chan)))
            break;
        if (f->frametype != AST_FRAME_VOICE || f->samples != SAMPPERFRAME)
            CONTINUEFREE;

        /* copy frame data to temporary buffer */
        samptotal += SAMPPERFRAME;
        ast_dsp_silence(dsp, f, &msofsilence);
        memcpy(data, f->data.ptr, SAMPPERFRAME * sizeof(short));

        /* write any pending repeat data back to caller in really cheap way.
         * if asterisk doesn't give us a steady flow of frames, like if
         * silence suppression is used, this application will get owned */
        if (out.cur < out.end) {
            f->delivery.tv_sec = 0;
            f->delivery.tv_usec = 0;
            if (out.end - out.cur < SAMPPERFRAME) {
                memset(f->data.ptr, 0, SAMPPERFRAME * sizeof(short));
                memcpy(f->data.ptr, (void *)out.cur, (void *)out.end - (void *)out.cur);
                out.end = out.cur = out.buf;
            } else {
                memcpy(f->data.ptr, (void *)out.cur, SAMPPERFRAME * sizeof(short));
                out.cur += SAMPPERFRAME;
            }
#ifdef _LIBSOUNDTOUCH4C_
            if (ops->pitch != 0.0) {
                SoundTouch_putSamples(st, f->data, SAMPPERFRAME);
                memset(f->data.ptr, 0, SAMPPERFRAME * sizeof(short));
                f->samples = SoundTouch_receiveSamplesEx(st, f->data, SAMPPERFRAME);
            }
#endif
            if (ast_write(chan, f) == -1)
                BREAKFREE;
        }

        /* in silence mode, we wait for non-silence */
        if (mode == MODE_SILENCE) {
            if (msofsilence)
                CONTINUEFREE;
            in.end = in.cur = in.buf;
            mode = MODE_RECORD;
            if (option_verbose >= 6)
                ast_verbose(VERBOSE_PREFIX_4 "Record(%ld)\n",
                            (unsigned long)(samptotal / 8));
        }

        /* have we run out of in buffer? */
        if (in.buf + in.len - in.cur < SAMPPERFRAME) {
            if ((ops->options & OPT_PARROT_SOUNDCLIPS) == OPT_PARROT_SOUNDCLIPS) {
                /* play random clip */
                ast_frfree(f);
                mode = MODE_SILENCE;
                in.end = in.cur = in.buf;
                out.end = out.cur = out.buf;
                if (ast_streamfile(chan, ops->soundclips[rand() % ops->soundclipcnt], chan->language) == -1)
                    goto OHSNAP;
                if (ast_waitstream(chan, "") == -1)
                    goto OHSNAP;
                ast_stopstream(chan);
                continue;
            }
            memcpy((void *)out.buf, (void *)in.buf, (void *)in.end - (void *)in.buf);
            out.cur = out.buf;
            out.end = out.buf + (in.end - in.buf);
            in.end = in.cur = in.buf;
            mode = MODE_SILENCE;
            if (option_verbose >= 6)
                ast_verbose(VERBOSE_PREFIX_4 "Repeat(%ld): %d so far (ran out of buffer)\n",
                            (unsigned long)(samptotal / 8),
                            (int)((out.end - out.buf) / 8));
        } else {
            memcpy((void *)in.cur, (void *)data, SAMPPERFRAME * sizeof(short));
            in.cur += SAMPPERFRAME;
        }

        /* in record mode, we slam more data on the 'in' buffer */
        if (mode == MODE_RECORD) {
            if (msofsilence < 200) {
                in.end = in.cur;

                if (ops->interrupt &&                          /* interrupting is enabled       */
                    out.cur < out.end &&                       /* there is voice being repeated */
                    (in.end - in.buf) / 8 >= ops->interrupt) { /* there is enough chatter       */
                    out.end = out.cur = out.buf;
                    if (option_verbose >= 6)
                        ast_verbose(VERBOSE_PREFIX_4 "Repeat(%ld): Interrupted\n",
                                    (unsigned long)(samptotal / 8));
                }
            } else {
                if ((in.end - in.buf) / 8 < ops->mintalk) {
                    if (option_verbose >= 6)
                        ast_verbose(VERBOSE_PREFIX_4 "Silence(%ld): %d not enough\n",
                                    (unsigned long)(samptotal / 8),
                                    (int)((in.end - in.buf) / 8));
                    mode = MODE_SILENCE;
                    CONTINUEFREE;
                } else {
                    in.end -= msofsilence * 8;
                    mode = MODE_PENDING;
                    if (option_verbose >= 6)
                        ast_verbose(VERBOSE_PREFIX_4 "Pending(%ld): %d so far\n",
                                    (unsigned long)(samptotal / 8),
                                    (int)((in.end - in.buf) / 8));
                }
            }
            CONTINUEFREE;
        }

        if (mode == MODE_PENDING) {
            if (!msofsilence) {
                in.end = in.cur;
                mode = MODE_RECORD;
                if (option_verbose >= 6)
                    ast_verbose(VERBOSE_PREFIX_4 "Record(%ld)\n",
                                (unsigned long)(samptotal / 8));
                CONTINUEFREE;
            } else if (msofsilence >= ops->wait) {
                if (!greeted && (ops->options & OPT_PARROT_GREETCLIPS) == OPT_PARROT_GREETCLIPS) {
                    /* greeting time */
                    ast_frfree(f);
                    mode = MODE_SILENCE;
                    in.end = in.cur = in.buf;
                    out.end = out.cur = out.buf;
                    greeted = 1;
                    if (ast_streamfile(chan, ops->greetclips[rand() % ops->greetclipcnt], chan->language) == -1)
                        goto OHSNAP;
                    if (ast_waitstream(chan, "") == -1)
                        goto OHSNAP;
                    ast_stopstream(chan);
                    continue;
                } else {
                    /* add their recording to out so it gets repeated */
                    /* add 300 ms of the silence to smooth end of voice */
                    in.end += msofsilence < 300 ? msofsilence * 8 : 300 * 8;
                    if (in.end > in.cur)
                        in.end = in.cur;
                    memcpy((void *)out.buf, (void *)in.buf, (void *)in.end - (void *)in.buf);
                    out.cur = out.buf;
                    out.end = out.buf + (in.end - in.buf);
                    in.end = in.cur = in.buf;
                    mode = MODE_SILENCE;
                    if (option_verbose >= 6)
                        ast_verbose(VERBOSE_PREFIX_4 "Repeat(%ld): %d to say after %d of silence\n",
                                    (unsigned long)(samptotal / 8),
                                    (int)((out.end - out.buf) / 8),
                                    (int)(msofsilence));
                }
            } else
                CONTINUEFREE;
        }

        CONTINUEFREE;
    }

OHSNAP:
    ast_dsp_free(dsp);
    free(in.buf);
    free(out.buf);
#ifdef _LIBSOUNDTOUCH4C_
    soundtouch_free(st);
#endif

    return 0;
}

static int parrot(struct ast_channel *chan, const struct parrot_ops *ops)
{
    int rc;
    struct ast_module_user *u;
    u = ast_module_user_add(chan);
    rc = parrot_main(chan, ops);
    ast_module_user_remove(u);
    return rc;
}

static int parrot_app_exec(struct ast_channel *chan, const char *data)
{
    AST_DECLARE_APP_ARGS(args,
                         AST_APP_ARG(options);
        );
    char *parse;
    struct parrot_ops ops;
    struct ast_flags opts = { 0, };
    char *opt_args[OPT_ARG_PARROT_ARRAY_SIZE];

    srand(time(NULL));

    memset(&ops, 0, sizeof(ops));
    ops.threshold = 256;
    ops.wait = 1000;
    ops.mintalk = 400;
    ops.maxtalk = 6000;
    ops.interrupt = 400;
    ops.pitch = 0.0;

    if (!ast_strlen_zero(data)) {
        parse = ast_strdupa(data);
        AST_STANDARD_APP_ARGS(args, parse);

        if (ast_app_parse_options(parrot_options, &opts, opt_args, args.options))
            return -1;

        if (ast_test_flag(&opts, OPT_PARROT_THRESHOLD) && !ast_strlen_zero(opt_args[OPT_ARG_PARROT_THRESHOLD])) {
            ops.options |= OPT_PARROT_THRESHOLD;
            ops.threshold = strtol(opt_args[OPT_ARG_PARROT_THRESHOLD], NULL, 10);
        }
        if (ast_test_flag(&opts, OPT_PARROT_WAIT) && !ast_strlen_zero(opt_args[OPT_ARG_PARROT_WAIT])) {
            ops.options |= OPT_PARROT_WAIT;
            ops.wait = strtol(opt_args[OPT_ARG_PARROT_WAIT], NULL, 10);
        }
        if (ast_test_flag(&opts, OPT_PARROT_MINTALK) && !ast_strlen_zero(opt_args[OPT_ARG_PARROT_MINTALK])) {
            ops.options |= OPT_PARROT_MINTALK;
            ops.mintalk = strtol(opt_args[OPT_ARG_PARROT_MINTALK], NULL, 10);
        }
        if (ast_test_flag(&opts, OPT_PARROT_MAXTALK) && !ast_strlen_zero(opt_args[OPT_ARG_PARROT_MAXTALK])) {
            ops.options |= OPT_PARROT_MAXTALK;
            ops.maxtalk = strtol(opt_args[OPT_ARG_PARROT_MAXTALK], NULL, 10);
        }
        if (ast_test_flag(&opts, OPT_PARROT_SOUNDCLIPS) && !ast_strlen_zero(opt_args[OPT_ARG_PARROT_SOUNDCLIPS])) {
            char *s;
            int n;
            ops.options |= OPT_PARROT_SOUNDCLIPS;
            s = ast_strdupa(opt_args[OPT_ARG_PARROT_SOUNDCLIPS]);
            for (n = 0; n < PARROT_MAX_CLIPS; n++) {
                ops.soundclips[n] = strsep(&s, "^");
                if (ops.soundclips[n])
                    ops.soundclipcnt++;
                else
                    break;
            }
        }
        if (ast_test_flag(&opts, OPT_PARROT_GREETCLIPS) && !ast_strlen_zero(opt_args[OPT_ARG_PARROT_GREETCLIPS])) {
            char *s;
            int n;
            ops.options |= OPT_PARROT_GREETCLIPS;
            s = ast_strdupa(opt_args[OPT_ARG_PARROT_GREETCLIPS]);
            for (n = 0; n < PARROT_MAX_CLIPS; n++) {
                ops.greetclips[n] = strsep(&s, "^");
                if (ops.greetclips[n])
                    ops.greetclipcnt++;
                else
                    break;
            }
        }
        if (ast_test_flag(&opts, OPT_PARROT_HANGUP) && !ast_strlen_zero(opt_args[OPT_ARG_PARROT_HANGUP])) {
            ops.options |= OPT_PARROT_HANGUP;
            ops.hangup = strtol(opt_args[OPT_ARG_PARROT_HANGUP], NULL, 10);
        }
        if (ast_test_flag(&opts, OPT_PARROT_INTERRUPT) && !ast_strlen_zero(opt_args[OPT_ARG_PARROT_INTERRUPT])) {
            ops.options |= OPT_PARROT_INTERRUPT;
            ops.hangup = strtol(opt_args[OPT_ARG_PARROT_INTERRUPT], NULL, 10);
        }
#ifdef _LIBSOUNDTOUCH4C_
        if (ast_test_flag(&opts, OPT_PARROT_PITCH) && !ast_strlen_zero(opt_args[OPT_ARG_PARROT_PITCH])) {
            ops.options |= OPT_PARROT_PITCH;
            ops.pitch = strtof(opt_args[OPT_ARG_PARROT_PITCH], NULL);
        }
#endif
    }

    return parrot(chan, &ops);
}

static int load_module()
{
    return ast_register_application(parrot_app, parrot_app_exec, parrot_synopsis, parrot_descrip);
}

static int unload_module()
{
    ast_module_user_hangup_all();
    return ast_unregister_application(parrot_app);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Phone Parrot Application");

/* For Emacs:
 * Local Variables:
 * indent-tabs-mode:nil
 * tab-width:4
 * c-basic-offset:4
 * c-file-style:nil
 * End:
 * For VIM:
 * vim:set expandtab softtabstop=4 shiftwidth=4 tabstop=4:
 */
