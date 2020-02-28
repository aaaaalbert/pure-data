/* Copyright (c) 1997-1999 Miller Puckette. Updated 2019 Dan Wilcox.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* this file contains, first, a collection of soundfile access routines, a
sort of soundfile library.  Second, the "soundfiler" object is defined which
uses the routines to read or write soundfiles, synchronously, from garrays.
These operations are not to be done in "real time" as they may have to wait
for disk accesses (even the write routine.)  Finally, the realtime objects
readsf~ and writesf~ are defined which confine disk operations to a separate
thread so that they can be used in real time.  The readsf~ and writesf~
objects use Posix-like threads. */

#include "d_soundfile.h"
#ifdef _WIN32
#include <io.h>
#endif
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>

/* Supported sample formats: LPCM (16 or 24 bit int) & 32 bit float */

/* TODO: add support for 32 bit int samples? */

#define MAXSFCHANS 64

/* GLIBC large file support */
#ifdef _LARGEFILE64_SOURCE
#define open open64
#endif

/* MSVC uses different naming for these */
#ifdef _MSC_VER
#define O_CREAT  _O_CREAT
#define O_TRUNC  _O_TRUNC
#define O_WRONLY _O_WRONLY
#endif

#define SCALE (1. / (1024. * 1024. * 1024. * 2.))

#define TYPENAME(type) type->t_name->s_name

    /* float sample conversion wrapper */
typedef union _floatuint {
  float f;
  uint32_t ui;
} t_floatuint;

/* ----- soundfile ----- */

void soundfile_clear(t_soundfile *sf)
{
    memset(sf, 0, sizeof(t_soundfile));
    sf->sf_fd = -1;
    sf->sf_bytelimit = SFMAXBYTES;
}

void soundfile_clearinfo(t_soundfile *sf)
{
    sf->sf_samplerate = 0;
    sf->sf_nchannels = 0;
    sf->sf_bytespersample = 0;
    sf->sf_headersize = 0;
    sf->sf_bigendian = 0;
    sf->sf_bytesperframe = 0;
    sf->sf_bytelimit = SFMAXBYTES;
}

void soundfile_copy(t_soundfile *dst, const t_soundfile *src)
{
    memcpy((char *)dst, (char *)src, sizeof(t_soundfile));
}

void soundfile_print(const t_soundfile *sf)
{
    printf("%d %d %d %ld %s %ld %d\n", sf->sf_samplerate, sf->sf_nchannels,
        sf->sf_bytespersample, sf->sf_headersize,
        (sf->sf_bigendian ? "b" : "l"), sf->sf_bytelimit,
        sf->sf_bytesperframe);
}

int soundfile_needsbyteswap(const t_soundfile *sf)
{
    return sf->sf_bigendian != sys_isbigendian();
}

const char* soundfile_strerror(int errnum, const t_soundfile *sf)
{
    switch (errnum)
    {
        case SOUNDFILE_ERR_SAMPLEFMT:
            return "supported sample formats: uncompressed "
                   "16 bit int, 24 bit int, or 32 bit float";
        default:
            if (sf && sf->sf_type && sf->sf_type->t_strerrorfn)
                return sf->sf_type->t_strerrorfn(errnum);
            else
                return strerror(errnum);
    }
}

    /** output soundfile format info as a list */
static void outlet_soundfileinfo(t_outlet *out, t_soundfile *sf)
{
    t_atom info_list[5];
    SETFLOAT((t_atom *)info_list, (t_float)sf->sf_samplerate);
    SETFLOAT((t_atom *)info_list+1,
        (t_float)(sf->sf_headersize < 0 ? 0 : sf->sf_headersize));
    SETFLOAT((t_atom *)info_list+2, (t_float)sf->sf_nchannels);
    SETFLOAT((t_atom *)info_list+3, (t_float)sf->sf_bytespersample);
    SETSYMBOL((t_atom *)info_list+4, gensym((sf->sf_bigendian ? "b" : "l")));
    outlet_list(out, &s_list, 5, (t_atom *)info_list);
}

    /* post system error, otherwise try to print type name and error str
       EIO is used as generic "couldn't read header" errnum */
static void object_readerror(const void *x, const char *header,
    const char *filename, int errnum, const t_soundfile *sf)
{
    if (errnum != EIO && errnum > 0) /* C/POSIX error */
        pd_error(x, "%s: %s: %s", header, filename, strerror(errnum));
    else if(sf->sf_type)
    {
            /* type implementation error? */
        pd_error(x, "%s: %s: unknown or bad header format (%s)",
            header, filename, TYPENAME(sf->sf_type));
        if (errnum != EIO && sf->sf_type->t_strerrorfn)
            error("%s", soundfile_strerror(errnum, sf));
    }
    else
        pd_error(x, "%s: %s: unknown or bad header format", header, filename);
}

/* ----- soundfile type ----- */

#define SFMAXTYPES 8 /**< enough room for now? */

/* TODO: should these globals be PERTHREAD? */

    /** supported type implementations */
static t_soundfile_type sf_types[SFMAXTYPES] = {0};

    /** number of types */
static size_t sf_numtypes = 0;

    /** min required header size, largest among the current types */
static int sf_minheadersize = 0;

    /** printable type argument list,
       dash prepended and separated by spaces */
static char sf_typeargs[MAXPDSTRING] = {0};

    /** special read-only raw type */
static t_soundfile_type sf_rawtype = {0};

    /* built-in type implementations */
void soundfile_wave_setup();
void soundfile_aiff_setup();
void soundfile_caf_setup();
void soundfile_next_setup();
void soundfile_raw_setup(t_soundfile_type *type);

    /** set up built-in types*/
void soundfile_type_setup()
{
    soundfile_wave_setup(); /* default first */
    soundfile_aiff_setup();
    soundfile_caf_setup();
    soundfile_next_setup();
    soundfile_raw_setup(&sf_rawtype); /* not added to sf_types */
}

int soundfile_addtype(t_soundfile_type *type)
{
    int i;
    if (sf_numtypes == SFMAXTYPES)
    {
        error("soundfile: max number of type implementations reached");
        return 0;
    }
    memcpy(&sf_types[sf_numtypes], type, sizeof(t_soundfile_type));
    sf_numtypes++;
    if (type->t_minheadersize > sf_minheadersize)
        sf_minheadersize = type->t_minheadersize;
    strcat(sf_typeargs, (sf_numtypes > 1 ? " -" : "-"));
    strcat(sf_typeargs, TYPENAME(type));
    return 1;
}

    /** return type list head */
static t_soundfile_type *soundfile_firsttype() {
    return &sf_types[0];
}

    /** return next type or NULL if at the end */
static t_soundfile_type *soundfile_nexttype(t_soundfile_type *type) {
    if (type == &sf_types[sf_numtypes-1])
        return NULL;
    return ++type;
}

/* ----- default implementations ----- */

int soundfile_type_open(t_soundfile *sf, int fd)
{
    sf->sf_fd = fd;
    return 1;
}

int soundfile_type_close(t_soundfile *sf)
{
    if (sf->sf_fd >= 0)
        sys_close(sf->sf_fd);
    sf->sf_fd = -1;
    return 1;
}

int soundfile_type_seektoframe(t_soundfile *sf, size_t frame)
{
    off_t offset = sf->sf_headersize +
        (sf->sf_bytesperframe * frame);
    if (lseek(sf->sf_fd, offset, 0) != offset)
        return 0;
    return 1;
}

ssize_t soundfile_type_readsamples(t_soundfile *sf,
    unsigned char *buf, size_t size)
{
    return read(sf->sf_fd, buf, size);
}

ssize_t soundfile_type_writesamples(t_soundfile *sf,
    const unsigned char *buf, size_t size)
{
    return write(sf->sf_fd, buf, size);
}

/* ----- read write ----- */

ssize_t fd_read(int fd, off_t offset, void *dst, size_t size)
{
    if (lseek(fd, offset, SEEK_SET) != offset)
        return -1;
    return read(fd, dst, size);
}

ssize_t fd_write(int fd, off_t offset, const void *src, size_t size)
{
    if (lseek(fd, offset, SEEK_SET) != offset)
        return -1;
    return write(fd, src, size);
}

/* ----- byte swappers ----- */

int sys_isbigendian(void)
{
    unsigned short s = 1;
    unsigned char c = *(char *)(&s);
    return (c == 0);
}

uint64_t swap8(uint64_t n, int doit)
{
    if (doit)
        return (((n >> 56) & 0x00000000000000ffULL) |
                ((n >> 40) & 0x000000000000ff00ULL) |
                ((n >> 24) & 0x0000000000ff0000ULL) |
                ((n >>  8) & 0x00000000ff000000ULL) |
                ((n <<  8) & 0x000000ff00000000ULL) |
                ((n << 24) & 0x0000ff0000000000ULL) |
                ((n << 40) & 0x00ff000000000000ULL) |
                ((n << 56) & 0xff00000000000000ULL));
    return n;
}

int64_t swap8s(int64_t n, int doit)
{
    if (doit)
    {
        n = ((n <<  8) & 0xff00ff00ff00ff00ULL) |
            ((n >>  8) & 0x00ff00ff00ff00ffULL);
        n = ((n << 16) & 0xffff0000ffff0000ULL) |
            ((n >> 16) & 0x0000ffff0000ffffULL );
        return (n << 32) | ((n >> 32) & 0xffffffffULL);
    }
    return n;
}

uint32_t swap4(uint32_t n, int doit)
{
    if (doit)
        return (((n & 0x0000ff) << 24) | ((n & 0x0000ff00) <<  8) |
                ((n & 0xff0000) >>  8) | ((n & 0xff000000) >> 24));
    return n;
}

int32_t swap4s(int32_t n, int doit)
{
    if (doit)
    {
        n = ((n << 8) & 0xff00ff00) | ((n >> 8) & 0xff00ff);
        return (n << 16) | ((n >> 16) & 0xffff);
    }
    return n;
}

uint16_t swap2(uint16_t n, int doit)
{
    if (doit)
        return (((n & 0x00ff) << 8) | ((n & 0xff00) >> 8));
    return n;
}

void swapstring4(char *foo, int doit)
{
    if (doit)
    {
        char a = foo[0], b = foo[1], c = foo[2], d = foo[3];
        foo[0] = d; foo[1] = c; foo[2] = b; foo[3] = a;
    }
}

void swapstring8(char *foo, int doit)
{
    if (doit)
    {
        char a = foo[0], b = foo[1], c = foo[2], d = foo[3],
        e = foo[4], f = foo[5], g = foo[6], h = foo[7];
        foo[0] = h; foo[1] = g; foo[2] = f; foo[3] = e;
        foo[4] = d; foo[5] = c; foo[6] = b; foo[7] = a;
    }
}

/* ----------------------- soundfile access routines ----------------------- */

    /** This routine opens a file, looks for a supported file format
        header, seeks to end of it, and fills in the soundfile header info
        values. Only 2- and 3-byte fixed-point samples and 4-byte floating point
        samples are supported.  If sf->sf_headersize is nonzero, the caller
        should supply the number of channels, endinanness, and bytes per sample;
        the header is ignored.  If sf->sf_type is non-NULL, the given type
        implementation is used. Otherwise, the routine tries to read the header
        and fill in the properties. Fills sf struct on success, closes fd on
        failure. */
int open_soundfile_via_fd(int fd, t_soundfile *sf, size_t skipframes)
{
    off_t offset;
    errno = 0;
    if (sf->sf_headersize >= 0) /* header detection overridden */
        sf->sf_type = &sf_rawtype;
    else
    {
        char buf[SFHDRBUFSIZE];
        ssize_t bytesread = read(fd, buf, sf_minheadersize);

        if (!sf->sf_type)
        {
                /* check header for type */
            t_soundfile_type *type = soundfile_firsttype();
            while (type)
            {
                if (type->t_isheaderfn(buf, bytesread))
                    break;
                type = soundfile_nexttype(type);
            }
            if (!type) /* not recognized */
                goto badheader;
            sf->sf_type = type;
        }
        else
        {
                /* check header using given type */
            if (!sf->sf_type->t_isheaderfn(buf, bytesread))
                goto badheader;
        }

            /* rewind and read header */
        if (lseek(fd, 0, SEEK_SET) < 0)
            goto badheader;
    }

        /* read header */
    if (!sf->sf_type->t_openfn(sf, fd))
        goto badheader;
    if (!sf->sf_type->t_readheaderfn(sf))
        goto badheader;

        /* seek past header and any sample frames to skip */
    if (!sf->sf_type->t_seektoframefn(sf, skipframes))
        goto badheader;
    sf->sf_bytelimit -= sf->sf_bytesperframe * skipframes;
    if (sf->sf_bytelimit < 0)
        sf->sf_bytelimit = 0;

        /* copy sample format back to caller */
    return fd;

badheader:
        /* the header wasn't recognized.  We're threadable here so let's not
        print out the error... */
    if (!errno) errno = EIO;
    if (sf->sf_fd >= 0 && sf->sf_type)
    {
        sf->sf_type->t_closefn(sf);
        fd = -1;
    }
    sf->sf_fd = -1;
    if (fd >= 0)
        sys_close(fd);
    return -1;
}

    /** open a soundfile, using open_via_path().  This is used by readsf~ in
        a not-perfectly-threadsafe way.  LATER replace with a thread-hardened
        version of open_soundfile_via_canvas().
        returns number of frames in the soundfile */
int open_soundfile_via_path(const char *dirname, const char *filename,
    t_soundfile *sf, size_t skipframes)
{
    char buf[MAXPDSTRING], *dummy;
    int fd, sf_fd;
    fd = open_via_path(dirname, filename, "", buf, &dummy, MAXPDSTRING, 1);
    if (fd < 0)
        return -1;
    sf_fd = open_soundfile_via_fd(fd, sf, skipframes);
    return sf_fd;
}

    /** open a soundfile, using open_via_canvas().  This is used by readsf~ in
        a not-perfectly-threadsafe way.  LATER replace with a thread-hardened
        version of open_soundfile_via_canvas().
        returns number of frames in the soundfile */
int open_soundfile_via_canvas(t_canvas *canvas, const char *filename,
    t_soundfile *sf, size_t skipframes)
{
    char buf[MAXPDSTRING], *dummy;
    int fd, sf_fd;
    fd = canvas_open(canvas, filename, "", buf, &dummy, MAXPDSTRING, 1);
    if (fd < 0)
        return -1;
    sf_fd = open_soundfile_via_fd(fd, sf, skipframes);
    return sf_fd;
}

static void soundfile_xferin_sample(const t_soundfile *sf, int nvecs,
    t_sample **vecs, size_t framesread, unsigned char *buf, size_t nframes)
{
    int nchannels = (sf->sf_nchannels < nvecs ? sf->sf_nchannels : nvecs), i;
    size_t j;
    unsigned char *sp, *sp2;
    t_sample *fp;
    for (i = 0, sp = buf; i < nchannels; i++, sp += sf->sf_bytespersample)
    {
        if (sf->sf_bytespersample == 2)
        {
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                        *fp = SCALE * ((sp2[0] << 24) | (sp2[1] << 16));
            }
            else
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                        *fp = SCALE * ((sp2[1] << 24) | (sp2[0] << 16));
            }
        }
        else if (sf->sf_bytespersample == 3)
        {
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                        *fp = SCALE * ((sp2[0] << 24) | (sp2[1] << 16) |
                                       (sp2[2] << 8));
            }
            else
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                        *fp = SCALE * ((sp2[2] << 24) | (sp2[1] << 16) |
                                       (sp2[0] << 8));
            }
        }
        else if (sf->sf_bytespersample == 4)
        {
            t_floatuint alias;
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                {
                    alias.ui = ((sp2[0] << 24) | (sp2[1] << 16) |
                                (sp2[2] << 8)  |  sp2[3]);
                    *fp = (t_sample)alias.f;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                {
                    alias.ui = ((sp2[3] << 24) | (sp2[2] << 16) |
                                (sp2[1] << 8)  |  sp2[0]);
                    *fp = (t_sample)alias.f;
                }
            }
        }
    }
        /* zero out other outputs */
    for (i = sf->sf_nchannels; i < nvecs; i++)
        for (j = nframes, fp = vecs[i]; j--;)
            *fp++ = 0;
}

static void soundfile_xferin_words(const t_soundfile *sf, int nvecs,
    t_word **vecs, size_t framesread, unsigned char *buf, size_t nframes)
{
    unsigned char *sp, *sp2;
    t_word *wp;
    int nchannels = (sf->sf_nchannels < nvecs ? sf->sf_nchannels : nvecs), i;
    size_t j;
    for (i = 0, sp = buf; i < nchannels; i++, sp += sf->sf_bytespersample)
    {
        if (sf->sf_bytespersample == 2)
        {
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                        wp->w_float = SCALE * ((sp2[0] << 24) | (sp2[1] << 16));
            }
            else
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                        wp->w_float = SCALE * ((sp2[1] << 24) | (sp2[0] << 16));
            }
        }
        else if (sf->sf_bytespersample == 3)
        {
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                        wp->w_float = SCALE * ((sp2[0] << 24) | (sp2[1] << 16) |
                                               (sp2[2] << 8));
            }
            else
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                        wp->w_float = SCALE * ((sp2[2] << 24) | (sp2[1] << 16) |
                                               (sp2[0] << 8));
            }
        }
        else if (sf->sf_bytespersample == 4)
        {
            t_floatuint alias;
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                {
                    alias.ui = ((sp2[0] << 24) | (sp2[1] << 16) |
                                (sp2[2] << 8)  |  sp2[3]);
                    wp->w_float = (t_float)alias.f;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                {
                    alias.ui = ((sp2[3] << 24) | (sp2[2] << 16) |
                                (sp2[1] << 8)  |  sp2[0]);
                    wp->w_float = (t_float)alias.f;
                }
            }
        }
    }
        /* zero out other outputs */
    for (i = sf->sf_nchannels; i < nvecs; i++)
        for (j = nframes, wp = vecs[i]; j--;)
            (wp++)->w_float = 0;
}

    /* soundfiler_write ...

       usage: write [flags] filename table ...
       flags:
         -nframes <frames>
         -skip <frames>
         -bytes <bytes per sample>
         -rate / -r <samplerate>
         -normalize
         -wave
         -aiff
         -caf
         -next / -nextstep
         -big
         -little
         -- (stop parsing flags)
    */

#define SFMAXWRITEMETA 8 /**< max write args meta messages */

    /** parsed write arguments */
typedef struct _soundfiler_writeargs
{
    t_symbol *wa_filesym;              /* file path symbol */
    t_soundfile_type *wa_type;         /* type implementation */
    int wa_samplerate;                 /* sample rate */
    int wa_bytespersample;             /* number of bytes per sample */
    int wa_bigendian;                  /* is sample data bigendian? */
    size_t wa_nframes;                 /* number of sample frames to write */
    size_t wa_onsetframes;             /* sample frame onset when writing */
    int wa_normalize;                  /* normalize samples? */
    int wa_nmeta;                      /* number of meta messages */
    struct
    {
        int argc;
        t_atom *argv;
    } wa_meta[8];                      /* meta message arguments */
} t_soundfiler_writeargs;

/* the routine which actually does the work should LATER also be called
from garray_write16. */

    /** Parse arguments for writing.  The "obj" argument is only for flagging
        errors.  For streaming to a file the "normalize", "onset" and "nframes"
        arguments shouldn't be set but the calling routine flags this. */
static int soundfiler_parsewriteargs(void *obj, int *p_argc, t_atom **p_argv,
    t_soundfiler_writeargs *wa)
{
    int argc = *p_argc;
    t_atom *argv = *p_argv;
    int samplerate = -1, bytespersample = 2, bigendian = 0, endianness = -1;
    size_t nframes = SFMAXFRAMES, onsetframes = 0;
    int normalize = 0;
    t_symbol *filesym;
    t_soundfile_type *type = NULL;

    while (argc > 0 && argv->a_type == A_SYMBOL &&
        *argv->a_w.w_symbol->s_name == '-')
    {
        const char *flag = argv->a_w.w_symbol->s_name + 1;
        if (!strcmp(flag, "skip"))
        {
            if (argc < 2 || argv[1].a_type != A_FLOAT ||
                ((onsetframes = argv[1].a_w.w_float) < 0))
                    return -1;
            argc -= 2; argv += 2;
        }
        else if (!strcmp(flag, "nframes"))
        {
            if (argc < 2 || argv[1].a_type != A_FLOAT ||
                ((nframes = argv[1].a_w.w_float) < 0))
                    return -1;
            argc -= 2; argv += 2;
        }
        else if (!strcmp(flag, "bytes"))
        {
            if (argc < 2 || argv[1].a_type != A_FLOAT ||
                ((bytespersample = argv[1].a_w.w_float) < 2) ||
                    bytespersample > 4)
                        return -1;
            argc -= 2; argv += 2;
        }
        else if (!strcmp(flag, "normalize"))
        {
            normalize = 1;
            argc -= 1; argv += 1;
        }
        else if (!strcmp(flag, "big"))
        {
            endianness = 1;
            argc -= 1; argv += 1;
        }
        else if (!strcmp(flag, "little"))
        {
            endianness = 0;
            argc -= 1; argv += 1;
        }
        else if (!strcmp(flag, "rate") || !strcmp(flag, "r"))
        {
            if (argc < 2 || argv[1].a_type != A_FLOAT ||
                ((samplerate = argv[1].a_w.w_float) <= 0))
                    return -1;
            argc -= 2; argv += 2;
        }
        else if (!strcmp(flag, "meta"))
        {
                /* save meta args for use later */
            t_atom *v;
            int c = 0;
            argc -= 1; argv += 1;
            v = argv;
            while (argc > 0)
            {
                if (argv->a_type == A_SYMBOL &&
                    *argv->a_w.w_symbol->s_name == '-')
                    break;
                argc -= 1; argv+= 1;
                c++;
            }
            if (!c)
            {
                error("ignoring empty -meta flag");
                break;
            }
            if (wa->wa_nmeta == SFMAXWRITEMETA)
            {
                error("max -meta flags reached, ignoring");
                break;
            }
            wa->wa_meta[wa->wa_nmeta].argc = c;
            wa->wa_meta[wa->wa_nmeta].argv = v;
            wa->wa_nmeta++;
        }
        else if (!strcmp(flag, "-"))
        {
            argc -= 1; argv += 1;
            break;
        }
        else if (!strcmp(flag, "nextstep"))
        {
                /* handle old "-nextsep" alias */
            type = soundfile_firsttype();
            while (type) {
                if (!strcmp("next", TYPENAME(type)))
                    break;
                type = soundfile_nexttype(type);
            }
            argc -= 1; argv += 1;
        }
        else
        {
                /* check for type by name */
            type = soundfile_firsttype();
            while (type) {
                if (!strcmp(flag, TYPENAME(type)))
                    break; 
                type = soundfile_nexttype(type);
            }
            if (!type)
                return -1; /* unknown flag */
            argc -= 1; argv += 1;
        }
    }
    if (!argc || argv->a_type != A_SYMBOL)
        return -1;
    filesym = argv->a_w.w_symbol;

        /* deduce from filename extension? */
    if (!type)
    {
        type = soundfile_firsttype();
        while (type)
        {
            if (type->t_hasextensionfn(filesym->s_name, MAXPDSTRING))
                break;
            type = soundfile_nexttype(type);
        }
        if (!type)
            type = soundfile_firsttype(); /* default if unknown */
    }

        /* check requested endianness */
    bigendian = type->t_endiannessfn(endianness);
    if (endianness != -1 && endianness != bigendian)
    {
        error("%s: file forced to %s endian", TYPENAME(type),
            (bigendian ? "big" : "little"));
    }

        /* return to caller */
    argc--; argv++;
    *p_argc = argc;
    *p_argv = argv;
    wa->wa_filesym = filesym;
    wa->wa_type = type;
    wa->wa_samplerate = samplerate;
    wa->wa_bytespersample = bytespersample;
    wa->wa_bigendian = bigendian;
    wa->wa_nframes = nframes;
    wa->wa_onsetframes = onsetframes;
    wa->wa_normalize = normalize;
    return 0;
}

    /** sets sf fd & headerisze on success and returns fd or -1 on failure */
static int create_soundfile(t_canvas *canvas, const char *filename,
    t_soundfile *sf, size_t nframes)
{
    char filenamebuf[MAXPDSTRING], pathbuf[MAXPDSTRING];
    ssize_t headersize = -1;
    int fd;

        /* create file */
    strncpy(filenamebuf, filename, MAXPDSTRING);
    if (!sf->sf_type->t_hasextensionfn(filenamebuf, MAXPDSTRING-10))
        if (!sf->sf_type->t_addextensionfn(filenamebuf, MAXPDSTRING-10))
            return -1;
    filenamebuf[MAXPDSTRING-10] = 0; /* FIXME: what is the 10 for? */
    canvas_makefilename(canvas, filenamebuf, pathbuf, MAXPDSTRING);
    if ((fd = sys_open(pathbuf, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0)
        return -1;
    if (!sf->sf_type->t_openfn(sf, fd))
        goto badcreate;

        /* write header */
    headersize = sf->sf_type->t_writeheaderfn(sf, nframes);
    if (headersize < 0)
        goto badcreate;
    sf->sf_headersize = headersize;
    return fd;

badcreate:
    if (sf->sf_fd >= 0)
        sf->sf_type->t_closefn(sf);
    else
        sys_close(fd);
    return -1;
}

static void soundfile_finishwrite(void *obj, const char *filename,
    t_soundfile *sf, size_t nframes, size_t frameswritten)
{
    if (frameswritten >= nframes) return;
    if (nframes < SFMAXFRAMES)
        pd_error(obj, "soundfiler_write: %ld out of %ld frames written",
            frameswritten, nframes);
    if (sf->sf_type->t_updateheaderfn(sf, frameswritten))
        return;
    pd_error(obj, "soundfiler_write: %s: %s", filename, strerror(errno));
}

static void soundfile_xferout_sample(const t_soundfile *sf,
    t_sample **vecs, unsigned char *buf, size_t nframes, size_t onsetframes,
    t_sample normalfactor)
{
    int i;
    size_t j;
    unsigned char *sp, *sp2;
    t_sample *fp;
    for (i = 0, sp = buf; i < sf->sf_nchannels; i++,
        sp += sf->sf_bytespersample)
    {
        if (sf->sf_bytespersample == 2)
        {
            t_sample ff = normalfactor * 32768.;
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                {
                    int xx = 32768. + (*fp * ff);
                    xx -= 32768;
                    if (xx < -32767)
                        xx = -32767;
                    if (xx > 32767)
                        xx = 32767;
                    sp2[0] = (xx >> 8);
                    sp2[1] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                {
                    int xx = 32768. + (*fp * ff);
                    xx -= 32768;
                    if (xx < -32767)
                        xx = -32767;
                    if (xx > 32767)
                        xx = 32767;
                    sp2[1] = (xx >> 8);
                    sp2[0] = xx;
                }
            }
        }
        else if (sf->sf_bytespersample == 3)
        {
            t_sample ff = normalfactor * 8388608.;
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                {
                    int xx = 8388608. + (*fp * ff);
                    xx -= 8388608;
                    if (xx < -8388607)
                        xx = -8388607;
                    if (xx > 8388607)
                        xx = 8388607;
                    sp2[0] = (xx >> 16);
                    sp2[1] = (xx >> 8);
                    sp2[2] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                {
                    int xx = 8388608. + (*fp * ff);
                    xx -= 8388608;
                    if (xx < -8388607)
                        xx = -8388607;
                    if (xx > 8388607)
                        xx = 8388607;
                    sp2[2] = (xx >> 16);
                    sp2[1] = (xx >> 8);
                    sp2[0] = xx;
                }
            }
        }
        else if (sf->sf_bytespersample == 4)
        {
            t_floatuint f2;
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                {
                    f2.f = *fp * normalfactor;
                    sp2[0] = (f2.ui >> 24); sp2[1] = (f2.ui >> 16);
                    sp2[2] = (f2.ui >> 8);  sp2[3] = f2.ui;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
                {
                    f2.f = *fp * normalfactor;
                    sp2[3] = (f2.ui >> 24); sp2[2] = (f2.ui >> 16);
                    sp2[1] = (f2.ui >> 8);  sp2[0] = f2.ui;
                }
            }
        }
    }
}

static void soundfile_xferout_words(const t_soundfile *sf, t_word **vecs,
    unsigned char *buf, size_t nframes, size_t onsetframes,
    t_sample normalfactor)
{
    int i;
    size_t j;
    unsigned char *sp, *sp2;
    t_word *wp;
    for (i = 0, sp = buf; i < sf->sf_nchannels;
         i++, sp += sf->sf_bytespersample)
    {
        if (sf->sf_bytespersample == 2)
        {
            t_sample ff = normalfactor * 32768.;
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                {
                    int xx = 32768. + (wp->w_float * ff);
                    xx -= 32768;
                    if (xx < -32767)
                        xx = -32767;
                    if (xx > 32767)
                        xx = 32767;
                    sp2[0] = (xx >> 8);
                    sp2[1] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                {
                    int xx = 32768. + (wp->w_float * ff);
                    xx -= 32768;
                    if (xx < -32767)
                        xx = -32767;
                    if (xx > 32767)
                        xx = 32767;
                    sp2[1] = (xx >> 8);
                    sp2[0] = xx;
                }
            }
        }
        else if (sf->sf_bytespersample == 3)
        {
            t_sample ff = normalfactor * 8388608.;
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                {
                    int xx = 8388608. + (wp->w_float * ff);
                    xx -= 8388608;
                    if (xx < -8388607)
                        xx = -8388607;
                    if (xx > 8388607)
                        xx = 8388607;
                    sp2[0] = (xx >> 16);
                    sp2[1] = (xx >> 8);
                    sp2[2] = xx;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                {
                    int xx = 8388608. + (wp->w_float * ff);
                    xx -= 8388608;
                    if (xx < -8388607)
                        xx = -8388607;
                    if (xx > 8388607)
                        xx = 8388607;
                    sp2[2] = (xx >> 16);
                    sp2[1] = (xx >> 8);
                    sp2[0] = xx;
                }
            }
        }
        else if (sf->sf_bytespersample == 4)
        {
            t_floatuint f2;
            if (sf->sf_bigendian)
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                {
                    f2.f = wp->w_float * normalfactor;
                    sp2[0] = (f2.ui >> 24); sp2[1] = (f2.ui >> 16);
                    sp2[2] = (f2.ui >> 8);  sp2[3] = f2.ui;
                }
            }
            else
            {
                for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
                    j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
                {
                    f2.f = wp->w_float * normalfactor;
                    sp2[3] = (f2.ui >> 24); sp2[2] = (f2.ui >> 16);
                    sp2[1] = (f2.ui >> 8);  sp2[0] = f2.ui;
                }
            }
        }
    }
}

/* ----- soundfiler - reads and writes soundfiles to/from "garrays" ----- */

#define SAMPBUFSIZE 1024

static t_class *soundfiler_class;

typedef struct _soundfiler
{
    t_object x_obj;
    t_outlet *x_out2;
    t_canvas *x_canvas;
} t_soundfiler;

static t_soundfiler *soundfiler_new(void)
{
    t_soundfiler *x = (t_soundfiler *)pd_new(soundfiler_class);
    x->x_canvas = canvas_getcurrent();
    outlet_new(&x->x_obj, &s_float);
    x->x_out2 = outlet_new(&x->x_obj, &s_float);
    return x;
}

static int soundfiler_readascii(t_soundfiler *x, const char *filename,
    int narray, t_garray **garrays, t_word **vecs, int resize, int finalsize)
{
    t_binbuf *b = binbuf_new();
    int n, i, j, nframes, vecsize;
    t_atom *atoms, *ap;
    if (binbuf_read_via_canvas(b, filename, x->x_canvas, 0))
        return 0;
    n = binbuf_getnatom(b);
    atoms = binbuf_getvec(b);
    nframes = n / narray;
#ifdef DEBUG_SOUNDFILE
    post("read 1 %d", n);
#endif
    if (nframes < 1)
    {
        pd_error(x, "soundfiler_read: %s: empty or very short file", filename);
        return 0;
    }
    if (resize)
    {
        for (i = 0; i < narray; i++)
        {
            garray_resize_long(garrays[i], nframes);
            garray_getfloatwords(garrays[i], &vecsize, &vecs[i]);
        }
    }
    else if (finalsize < nframes)
        nframes = finalsize;
#ifdef DEBUG_SOUNDFILE
    post("read 2");
#endif
    for (j = 0, ap = atoms; j < nframes; j++)
        for (i = 0; i < narray; i++)
            vecs[i][j].w_float = atom_getfloat(ap++);
        /* zero out remaining elements of vectors */
    for (i = 0; i < narray; i++)
    {
        int vecsize;
        if (garray_getfloatwords(garrays[i], &vecsize, &vecs[i]))
            for (j = nframes; j < vecsize; j++)
                vecs[i][j].w_float = 0;
    }
    for (i = 0; i < narray; i++)
        garray_redraw(garrays[i]);
#ifdef DEBUG_SOUNDFILE
    post("read 3");
#endif
    return nframes;
}

    /* soundfiler_read ...

       usage: read [flags] filename table ...
       flags:
           -skip <frames> ... frames to skip in file
           -onset <frames> ... onset in table to read into (NOT DONE YET)
           -raw <headersize channels bytes endian>
           -resize
           -maxsize <max-size>
           -ascii
           -- (stop parsing flags)
    */

static void soundfiler_read(t_soundfiler *x, t_symbol *s,
    int argc, t_atom *argv)
{
    t_soundfile sf = {0};
    int fd = -1, resize = 0, ascii = 0, meta = 0, i;
    size_t skipframes = 0, finalsize = 0, maxsize = SFMAXFRAMES,
           framesread = 0, bufframes, j;
    ssize_t nframes, framesinfile;
    char endianness;
    const char *filename;
    t_garray *garrays[MAXSFCHANS];
    t_word *vecs[MAXSFCHANS];
    char sampbuf[SAMPBUFSIZE];

    soundfile_clear(&sf);
    sf.sf_headersize = -1;
    while (argc > 0 && argv->a_type == A_SYMBOL &&
        *argv->a_w.w_symbol->s_name == '-')
    {
        const char *flag = argv->a_w.w_symbol->s_name + 1;
        if (!strcmp(flag, "skip"))
        {
            if (argc < 2 || argv[1].a_type != A_FLOAT ||
                ((skipframes = argv[1].a_w.w_float) < 0))
                    goto usage;
            argc -= 2; argv += 2;
        }
        else if (!strcmp(flag, "ascii"))
        {
            if (sf.sf_headersize >= 0)
                post("soundfiler_read: '-raw' overridden by '-ascii'");
            ascii = 1;
            argc--; argv++;
        }
        else if (!strcmp(flag, "raw"))
        {
            if (ascii)
                post("soundfiler_read: '-raw' overridden by '-ascii'");
            if (argc < 5 ||
                argv[1].a_type != A_FLOAT ||
                ((sf.sf_headersize = argv[1].a_w.w_float) < 0) ||
                argv[2].a_type != A_FLOAT ||
                ((sf.sf_nchannels = argv[2].a_w.w_float) < 1) ||
                (sf.sf_nchannels > MAXSFCHANS) ||
                argv[3].a_type != A_FLOAT ||
                ((sf.sf_bytespersample = argv[3].a_w.w_float) < 2) ||
                    (sf.sf_bytespersample > 4) ||
                argv[4].a_type != A_SYMBOL ||
                    ((endianness = argv[4].a_w.w_symbol->s_name[0]) != 'b'
                    && endianness != 'l' && endianness != 'n'))
                        goto usage;
            if (endianness == 'b')
                sf.sf_bigendian = 1;
            else if (endianness == 'l')
                sf.sf_bigendian = 0;
            else
                sf.sf_bigendian = sys_isbigendian();
            sf.sf_samplerate = sys_getsr();
            sf.sf_bytesperframe = sf.sf_nchannels * sf.sf_bytespersample;
            argc -= 5; argv += 5;
        }
        else if (!strcmp(flag, "resize"))
        {
            resize = 1;
            argc -= 1; argv += 1;
        }
        else if (!strcmp(flag, "maxsize"))
        {
            if (argc < 2 || argv[1].a_type != A_FLOAT ||
                ((maxsize = (argv[1].a_w.w_float > SFMAXFRAMES ?
                SFMAXFRAMES : argv[1].a_w.w_float)) < 0))
                    goto usage;
            resize = 1;     /* maxsize implies resize. */
            argc -= 2; argv += 2;
        }
        else if (!strcmp(flag, "meta"))
        {
            meta = 1;
            argc -= 1; argv += 1;
        }
        else if (!strcmp(flag, "-"))
        {
            argc -= 1; argv += 1;
            break;
        }
        else
        {
                /* check for type by name */
            t_soundfile_type *type = soundfile_firsttype();
            while (type) {
                if (!strcmp(flag, TYPENAME(type)))
                    break;
                type = soundfile_nexttype(type);
            }
            if (!type)
                goto usage; /* unknown flag */
            sf.sf_type = type;
            argc -= 1; argv += 1;
        }
    }
    if (argc < 1 ||                           /* no filename or tables */
        argc > MAXSFCHANS + 1 ||              /* too many tables */
        argv[0].a_type != A_SYMBOL)           /* bad filename */
            goto usage;
    filename = argv[0].a_w.w_symbol->s_name;
    argc--; argv++;

    for (i = 0; i < argc; i++)
    {
        int vecsize;
        if (argv[i].a_type != A_SYMBOL)
            goto usage;
        if (!(garrays[i] =
            (t_garray *)pd_findbyclass(argv[i].a_w.w_symbol, garray_class)))
        {
            pd_error(x, "%s: no such table", argv[i].a_w.w_symbol->s_name);
            goto done;
        }
        else if (!garray_getfloatwords(garrays[i], &vecsize,
                &vecs[i]))
            error("%s: bad template for tabwrite",
                argv[i].a_w.w_symbol->s_name);
        if (finalsize && finalsize != vecsize && !resize)
        {
            post("soundfiler_read: arrays have different lengths; resizing...");
            resize = 1;
        }
        finalsize = vecsize;
    }
    if (ascii)
    {
        framesread = soundfiler_readascii(x, filename,
            argc, garrays, vecs, resize, finalsize);
        outlet_float(x->x_obj.ob_outlet, (t_float)framesread);
        return;
    }

    fd = open_soundfile_via_canvas(x->x_canvas, filename, &sf, skipframes);
    if (fd < 0)
    {
        object_readerror(x, "soundfiler_read", filename, errno, &sf);
        goto done;
    }
    framesinfile = sf.sf_bytelimit / sf.sf_bytesperframe;

        /* read meta data to outlet */
     if (meta && sf.sf_type->t_readmetafn)
         if (!sf.sf_type->t_readmetafn(&sf, x->x_out2))
             pd_error(x, "soundfiler_read: reading meta data failed");

    if (resize)
    {
            /* figure out what to resize to using header info */
        if (framesinfile > maxsize)
        {
            pd_error(x, "soundfiler_read: truncated to %ld elements", maxsize);
            framesinfile = maxsize;
        }
        finalsize = framesinfile;
        for (i = 0; i < argc; i++)
        {
            int vecsize;
            garray_resize_long(garrays[i], finalsize);
                /* for sanity's sake let's clear the save-in-patch flag here */
            garray_setsaveit(garrays[i], 0);
            if (!garray_getfloatwords(garrays[i], &vecsize, &vecs[i])
                /* if the resize failed, garray_resize reported the error */
                || (vecsize != framesinfile))
            {
                pd_error(x, "resize failed");
                goto done;
            }
        }
    }

    if (!finalsize) finalsize = SFMAXFRAMES;
    if (finalsize > framesinfile)
        finalsize = framesinfile;

        /* no tablenames, try to use header info instead of reading */
    if (argc == 0 &&
        !(sf.sf_type == &sf_rawtype || /* always read raw */
        finalsize == SFMAXFRAMES))     /* unknown, read */
    {
        framesread = finalsize;
        goto done;
    }

        /* read */
    bufframes = SAMPBUFSIZE / sf.sf_bytesperframe;
    for (framesread = 0; framesread < finalsize;)
    {
        size_t thisread = finalsize - framesread;
        thisread = (thisread > bufframes ? bufframes : thisread);
        nframes =
            sf.sf_type->t_readsamplesfn(&sf, (unsigned char *)sampbuf,
                thisread * sf.sf_bytesperframe) / sf.sf_bytesperframe;
        if (nframes <= 0) break;
        soundfile_xferin_words(&sf, argc, vecs, framesread,
            (unsigned char *)sampbuf, nframes);
        framesread += nframes;
    }

        /* zero out remaining elements of vectors */
    for (i = 0; i < argc; i++)
    {
        int vecsize;
        if (garray_getfloatwords(garrays[i], &vecsize, &vecs[i]))
            for (j = framesread; j < vecsize; j++)
                vecs[i][j].w_float = 0;
    }
        /* zero out vectors in excess of number of channels */
    for (i = sf.sf_nchannels; i < argc; i++)
    {
        int vecsize;
        t_word *foo;
        if (garray_getfloatwords(garrays[i], &vecsize, &foo))
            for (j = 0; j < vecsize; j++)
                foo[j].w_float = 0;
    }
        /* do all graphics updates */
    for (i = 0; i < argc; i++)
        garray_redraw(garrays[i]);
    goto done;
usage:
    pd_error(x, "usage: read [flags] filename [tablename]...");
    post("flags: -skip <n> -resize -maxsize <n> %s --...", sf_typeargs);
    post("-raw <headerbytes> <channels> <bytespersample> "
         "<endian (b, l, or n)>");
done:
    if (sf.sf_fd >= 0 && sf.sf_type)
    {
        sf.sf_type->t_closefn(&sf);
        fd = -1;
    }
    if (fd >= 0)
        sys_close(fd);
    outlet_soundfileinfo(x->x_out2, &sf);
    outlet_float(x->x_obj.ob_outlet, (t_float)framesread);
}

    /** this is broken out from soundfiler_write below so garray_write can
        call it too... not done yet though. */
size_t soundfiler_dowrite(void *obj, t_canvas *canvas,
    int argc, t_atom *argv, t_soundfile *sf)
{
    t_soundfiler_writeargs wa = {0};
    int fd = -1, i;
    size_t bufframes, frameswritten = 0, j;
    t_garray *garrays[MAXSFCHANS];
    t_word *vectors[MAXSFCHANS];
    char sampbuf[SAMPBUFSIZE];
    t_sample normfactor, biggest = 0;

    soundfile_clear(sf);
    if (soundfiler_parsewriteargs(obj, &argc, &argv, &wa))
        goto usage;
    sf->sf_type = wa.wa_type;
    sf->sf_nchannels = argc;
    sf->sf_samplerate = wa.wa_samplerate;
    sf->sf_bytespersample = wa.wa_bytespersample;
    sf->sf_bigendian = wa.wa_bigendian;
    sf->sf_bytesperframe = argc * wa.wa_bytespersample;
    if (sf->sf_nchannels < 1 || sf->sf_nchannels > MAXSFCHANS)
        goto usage;
    if (sf->sf_samplerate <= 0)
        sf->sf_samplerate = sys_getsr();
    for (i = 0; i < sf->sf_nchannels; i++)
    {
        int vecsize;
        if (argv[i].a_type != A_SYMBOL)
            goto usage;
        if (!(garrays[i] =
            (t_garray *)pd_findbyclass(argv[i].a_w.w_symbol, garray_class)))
        {
            pd_error(obj, "%s: no such table", argv[i].a_w.w_symbol->s_name);
            goto fail;
        }
        else if (!garray_getfloatwords(garrays[i], &vecsize, &vectors[i]))
            error("%s: bad template for tabwrite",
                argv[i].a_w.w_symbol->s_name);
        if (wa.wa_nframes > vecsize - wa.wa_onsetframes)
            wa.wa_nframes = vecsize - wa.wa_onsetframes;
    }
    if (wa.wa_nframes <= 0)
    {
        pd_error(obj, "soundfiler_write: no samples at onset %ld",
            wa.wa_onsetframes);
        goto fail;
    }
        /* find biggest sample for normalizing */
    for (i = 0; i < sf->sf_nchannels; i++)
    {
        for (j = wa.wa_onsetframes; j < wa.wa_nframes + wa.wa_onsetframes; j++)
        {
            if (vectors[i][j].w_float > biggest)
                biggest = vectors[i][j].w_float;
            else if (-vectors[i][j].w_float > biggest)
                biggest = -vectors[i][j].w_float;
        }
    }
    if ((fd = create_soundfile(canvas, wa.wa_filesym->s_name,
        sf, wa.wa_nframes)) < 0)
    {
        post("%s: %s\n", wa.wa_filesym->s_name, soundfile_strerror(errno, sf));
        goto fail;
    }
    if (!wa.wa_normalize)
    {
        if ((sf->sf_bytespersample != 4) && (biggest > 1))
        {
            post("%s: reducing max amplitude %f to 1",
                wa.wa_filesym->s_name, biggest);
            wa.wa_normalize = 1;
        }
        else post("%s: biggest amplitude = %f", wa.wa_filesym->s_name, biggest);
    }
    if (wa.wa_normalize)
        normfactor = (biggest > 0 ? 32767./(32768. * biggest) : 1);
    else normfactor = 1;
        /* write meta data */
    if (wa.wa_nmeta)
    {
        if (sf->sf_type->t_writemetafn)
        {
            int i;
            for (i = 0; i < wa.wa_nmeta; ++i)
            {
                if (!sf->sf_type->t_writemetafn(sf,
                        wa.wa_meta[i].argc, wa.wa_meta[i].argv))
                    pd_error(obj, "writesf: writing %s metadata failed",
                        TYPENAME(sf->sf_type));
            }
        }
        else
        {
            pd_error(obj,
                "soundfiler_write: %s does not support writing metadata",
                TYPENAME(sf->sf_type));
        }
    }
    bufframes = SAMPBUFSIZE / sf->sf_bytesperframe;
    for (frameswritten = 0; frameswritten < wa.wa_nframes;)
    {
        size_t thiswrite = wa.wa_nframes - frameswritten,
               byteswritten, datasize;
        thiswrite = (thiswrite > bufframes ? bufframes : thiswrite);
        datasize = sf->sf_bytesperframe * thiswrite;
        soundfile_xferout_words(sf, vectors, (unsigned char *)sampbuf,
            thiswrite, wa.wa_onsetframes, normfactor);
        byteswritten = sf->sf_type->t_writesamplesfn(sf,
            (const unsigned char*)sampbuf, datasize);
        if (byteswritten < datasize)
        {
            post("%s: %s", wa.wa_filesym->s_name, strerror(errno));
            if (byteswritten > 0)
                frameswritten += byteswritten / sf->sf_bytesperframe;
            break;
        }
        frameswritten += thiswrite;
        wa.wa_onsetframes += thiswrite;
    }
    if (fd >= 0)
    {
        soundfile_finishwrite(obj, wa.wa_filesym->s_name, sf,
                wa.wa_nframes, frameswritten);
        sf->sf_type->t_closefn(sf);
        fd = -1;
    }
    return frameswritten;
usage:
    pd_error(obj, "usage: write [flags] filename tablename...");
    post("flags: -skip <n> -nframes <n> -bytes <n> %s ...", sf_typeargs);
    post("-big -little -normalize -meta <type> [args...] --");
    post("(defaults to a 16 bit wave file)");
fail:
    if (sf->sf_fd >= 0 && sf->sf_type)
    {
        sf->sf_type->t_closefn(sf);
        fd = -1;
    }
    soundfile_clear(sf); /* clear any bad data */
    if (fd >= 0)
        sys_close(fd);
    return 0;
}

static void soundfiler_write(t_soundfiler *x, t_symbol *s,
    int argc, t_atom *argv)
{
    size_t frameswritten;
    t_soundfile sf = {0};
    frameswritten = soundfiler_dowrite(x, x->x_canvas, argc, argv, &sf);
    outlet_soundfileinfo(x->x_out2, &sf);
    outlet_float(x->x_obj.ob_outlet, (t_float)frameswritten);
}

    /* list supported types implementations */
static void soundfiler_list(t_soundfiler *x, t_symbol *s,
    int argc, t_atom *argv)
{
    int i;
    t_atom list[SFMAXTYPES];
    for (i = 0; i < sf_numtypes; i++)
        SETSYMBOL(&list[i], sf_types[i].t_name);
    outlet_list(x->x_obj.ob_outlet, &s_list, i, list);
}

static void soundfiler_setup(void)
{
    soundfiler_class = class_new(gensym("soundfiler"),
        (t_newmethod)soundfiler_new, 0,
        sizeof(t_soundfiler), 0, 0);
    class_addmethod(soundfiler_class, (t_method)soundfiler_read,
        gensym("read"), A_GIMME, 0);
    class_addmethod(soundfiler_class, (t_method)soundfiler_write,
        gensym("write"), A_GIMME, 0);
    class_addmethod(soundfiler_class, (t_method)soundfiler_list,
        gensym("list"), A_GIMME, 0);
}

/* ------------------------- readsf object ------------------------- */

/* READSF uses the Posix threads package; for the moment we're Linux
only although this should be portable to the other platforms.

Each instance of readsf~ owns a "child" thread for doing the Posix file reading.
The parent thread signals the child each time:
    (1) a file wants opening or closing;
    (2) we've eaten another 1/16 of the shared buffer (so that the
        child thread should check if it's time to read some more.)
The child signals the parent whenever a read has completed.  Signaling
is done by setting "conditions" and putting data in mutex-controlled common
areas.
*/

#define MAXVECSIZE 128

#define READSIZE 65536
#define WRITESIZE 65536
#define DEFBUFPERCHAN 262144
#define MINBUFSIZE (4 * READSIZE)
#define MAXBUFSIZE 16777216     /* arbitrary; just don't want to hang malloc */

    /* read/write thread request type */
typedef enum _soundfile_request
{
    REQUEST_NOTHING = 0,
    REQUEST_OPEN    = 1,
    REQUEST_CLOSE   = 2,
    REQUEST_QUIT    = 3,
    REQUEST_BUSY    = 4
} t_soundfile_request;

    /* read/write thread state */
typedef enum _soundfile_state
{
    STATE_IDLE    = 0,
    STATE_STARTUP = 1,
    STATE_STREAM  = 2
} t_soundfile_state;

static t_class *readsf_class;

typedef struct _readsf
{
    t_object x_obj;
    t_canvas *x_canvas;
    t_clock *x_clock;
    char *x_buf;                       /**< soundfile buffer */
    int x_bufsize;                     /**< buffer size in bytes */
    int x_noutlets;                    /**< number of audio outlets */
    t_sample *(x_outvec[MAXSFCHANS]);  /**< audio vectors */
    int x_vecsize;                     /**< vector size for transfers */
    t_outlet *x_bangout;               /**< bang-on-done outlet */
    t_soundfile_state x_state;         /**< opened, running, or idle */
    t_float x_insamplerate;            /**< input signal sample rate, if known */
        /* parameters to communicate with subthread */
    t_soundfile_request x_requestcode; /**< pending request to I/O thread */
    const char *x_filename;   /**< file to open (string permanently alloced) */
    int x_fileerror;          /**< slot for "errno" return */
    t_soundfile x_sf;         /**< soundfile fd, type, and format info */
    size_t x_onsetframes;     /**< number of sample frames to skip */
    int x_fifosize;           /**< buffer size appropriately rounded down */
    int x_fifohead;           /**< index of next byte to get from file */
    int x_fifotail;           /**< index of next byte the ugen will read */
    int x_eof;                /**< true if fifohead has stopped changing */
    int x_sigcountdown;       /**< counter for signaling child for more data */
    int x_sigperiod;          /**< number of ticks per signal */
    size_t x_frameswritten;   /**< writesf~ only; frames written */
    t_float x_f;              /**< writesf~ only; scalar for signal inlet */
    pthread_mutex_t x_mutex;
    pthread_cond_t x_requestcondition;
    pthread_cond_t x_answercondition;
    pthread_t x_childthread;
} t_readsf;

/* ----- the child thread which performs file I/O ----- */

#if 0
static void pute(const char *s)   /* debug routine */
{
    write(2, s, strlen(s));
}
#define DEBUG_SOUNDFILE
#endif

#if 1
#define sfread_cond_wait pthread_cond_wait
#define sfread_cond_signal pthread_cond_signal
#else
#include <sys/time.h>    /* debugging version... */
#include <sys/types.h>
static void readsf_fakewait(pthread_mutex_t *b)
{
    struct timeval timout;
    timout.tv_sec = 0;
    timout.tv_usec = 1000000;
    pthread_mutex_unlock(b);
    select(0, 0, 0, 0, &timout);
    pthread_mutex_lock(b);
}

#define sfread_cond_wait(a,b) readsf_fakewait(b)
#define sfread_cond_signal(a)
#endif

static void *readsf_child_main(void *zz)
{
    t_readsf *x = zz;
    t_soundfile sf = {0};
    soundfile_clear(&sf);
#ifdef DEBUG_SOUNDFILE
    pute("1\n");
#endif
    pthread_mutex_lock(&x->x_mutex);
    while (1)
    {
        int fifohead;
        char *buf;
#ifdef DEBUG_SOUNDFILE
        pute("0\n");
#endif
        if (x->x_requestcode == REQUEST_NOTHING)
        {
#ifdef DEBUG_SOUNDFILE
            pute("wait 2\n");
#endif
            sfread_cond_signal(&x->x_answercondition);
            sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
            pute("3\n");
#endif
        }
        else if (x->x_requestcode == REQUEST_OPEN)
        {
            char boo[80];
            ssize_t bytesread;
            size_t wantbytes;

                /* copy file stuff out of the data structure so we can
                relinquish the mutex while we're in open_soundfile_via_path() */
            size_t onsetframes = x->x_onsetframes;
            const char *filename = x->x_filename;
            const char *dirname = canvas_getdir(x->x_canvas)->s_name;

#ifdef DEBUG_SOUNDFILE
            pute("4\n");
#endif
                /* alter the request code so that an ensuing "open" will get
                noticed. */
            x->x_requestcode = REQUEST_BUSY;
            x->x_fileerror = 0;

                /* if there's already a file open, close it */
            if (x->x_sf.sf_fd >= 0)
            {
                pthread_mutex_unlock(&x->x_mutex);
                sf.sf_type->t_closefn(&sf);
                pthread_mutex_lock(&x->x_mutex);
                x->x_sf.sf_fd = -1;
                x->x_sf.sf_data = NULL; /* closefn freed this */
                if (x->x_requestcode != REQUEST_BUSY)
                    goto lost;
            }
                /* cache sf *after* closing as x->sf's fd, type, & data
                    may have changed in readsf_open() */
            soundfile_copy(&sf, &x->x_sf);

                /* open the soundfile with the mutex unlocked */
            pthread_mutex_unlock(&x->x_mutex);
            open_soundfile_via_path(dirname, filename, &sf, onsetframes);
            pthread_mutex_lock(&x->x_mutex);

#ifdef DEBUG_SOUNDFILE
            pute("5\n");
#endif
                /* copy back into the instance structure. */
            soundfile_copy(&x->x_sf, &sf);
            if (sf.sf_fd < 0)
            {
                x->x_fileerror = errno;
                x->x_eof = 1;
#ifdef DEBUG_SOUNDFILE
                pute("open failed\n");
                pute(filename);
                pute(dirname);
#endif
                goto lost;
            }
                /* check if another request has been made; if so, field it */
            if (x->x_requestcode != REQUEST_BUSY)
                goto lost;
#ifdef DEBUG_SOUNDFILE
            pute("6\n");
#endif
            x->x_fifohead = 0;
                    /* set fifosize from bufsize.  fifosize must be a
                    multiple of the number of bytes eaten for each DSP
                    tick.  We pessimistically assume MAXVECSIZE samples
                    per tick since that could change.  There could be a
                    problem here if the vector size increases while a
                    soundfile is being played...  */
            x->x_fifosize = x->x_bufsize - (x->x_bufsize %
                (x->x_sf.sf_bytesperframe * MAXVECSIZE));
                    /* arrange for the "request" condition to be signaled 16
                    times per buffer */
#ifdef DEBUG_SOUNDFILE
            sprintf(boo, "fifosize %d\n",
                x->x_fifosize);
            pute(boo);
#endif
            x->x_sigcountdown = x->x_sigperiod = (x->x_fifosize /
                (16 * x->x_sf.sf_bytesperframe * x->x_vecsize));
                /* in a loop, wait for the fifo to get hungry and feed it */

            while (x->x_requestcode == REQUEST_BUSY)
            {
                int fifosize = x->x_fifosize;
#ifdef DEBUG_SOUNDFILE
                pute("77\n");
#endif
                if (x->x_eof)
                    break;
                if (x->x_fifohead >= x->x_fifotail)
                {
                        /* if the head is >= the tail, we can immediately read
                        to the end of the fifo.  Unless, that is, we would
                        read all the way to the end of the buffer and the
                        "tail" is zero; this would fill the buffer completely
                        which isn't allowed because you can't tell a completely
                        full buffer from an empty one. */
                    if (x->x_fifotail || (fifosize - x->x_fifohead > READSIZE))
                    {
                        wantbytes = fifosize - x->x_fifohead;
                        if (wantbytes > READSIZE)
                            wantbytes = READSIZE;
                        if (wantbytes > x->x_sf.sf_bytelimit)
                            wantbytes = x->x_sf.sf_bytelimit;
#ifdef DEBUG_SOUNDFILE
                        sprintf(boo, "head %d, tail %d, size %ld\n",
                            x->x_fifohead, x->x_fifotail, wantbytes);
                        pute(boo);
#endif
                    }
                    else
                    {
#ifdef DEBUG_SOUNDFILE
                        pute("wait 7a...\n");
#endif
                        sfread_cond_signal(&x->x_answercondition);
#ifdef DEBUG_SOUNDFILE
                        pute("signaled\n");
#endif
                        sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
                        pute("7a done\n");
#endif
                        continue;
                    }
                }
                else
                {
                        /* otherwise check if there are at least READSIZE
                        bytes to read.  If not, wait and loop back. */
                    wantbytes =  x->x_fifotail - x->x_fifohead - 1;
                    if (wantbytes < READSIZE)
                    {
#ifdef DEBUG_SOUNDFILE
                        pute("wait 7...\n");
#endif
                        sfread_cond_signal(&x->x_answercondition);
                        sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
                        pute("7 done\n");
#endif
                        continue;
                    }
                    else wantbytes = READSIZE;
                    if (wantbytes > x->x_sf.sf_bytelimit)
                        wantbytes = x->x_sf.sf_bytelimit;
                }
#ifdef DEBUG_SOUNDFILE
                pute("8\n");
#endif
                sf.sf_fd = x->x_sf.sf_fd;
                buf = x->x_buf;
                fifohead = x->x_fifohead;
                pthread_mutex_unlock(&x->x_mutex);
                bytesread = sf.sf_type->t_readsamplesfn(&sf,
                    (unsigned char*)(buf + fifohead), wantbytes);
                pthread_mutex_lock(&x->x_mutex);
                if (x->x_requestcode != REQUEST_BUSY)
                    break;
                if (bytesread < 0)
                {
#ifdef DEBUG_SOUNDFILE
                    pute("fileerror\n");
#endif
                    x->x_fileerror = errno;
                    break;
                }
                else if (bytesread == 0)
                {
                    x->x_eof = 1;
                    break;
                }
                else
                {
                    x->x_fifohead += bytesread;
                    x->x_sf.sf_bytelimit -= bytesread;
                    if (x->x_fifohead == fifosize)
                        x->x_fifohead = 0;
                    if (x->x_sf.sf_bytelimit <= 0)
                    {
                        x->x_eof = 1;
                        break;
                    }
                }
#ifdef DEBUG_SOUNDFILE
                sprintf(boo, "after: head %d, tail %d\n",
                    x->x_fifohead, x->x_fifotail);
                pute(boo);
#endif
                    /* signal parent in case it's waiting for data */
                sfread_cond_signal(&x->x_answercondition);
            }

        lost:
            if (x->x_requestcode == REQUEST_BUSY)
                x->x_requestcode = REQUEST_NOTHING;
#ifdef DEBUG_SOUNDFILE
                pute("lost\n");
#endif
                /* fell out of read loop: close file if necessary,
                set EOF and signal once more */
            if (sf.sf_fd >= 0)
            {
                    /* use cached sf as x->sf's fd, type, & data
                    may have changed in readsf_open() */
                pthread_mutex_unlock(&x->x_mutex);
                sf.sf_type->t_closefn(&sf);
                pthread_mutex_lock(&x->x_mutex);
            }
            sfread_cond_signal(&x->x_answercondition);

        }
        else if (x->x_requestcode == REQUEST_CLOSE)
        {
            if (sf.sf_fd >= 0)
            {
                    /* use cached sf */
                pthread_mutex_unlock(&x->x_mutex);
                sf.sf_type->t_closefn(&sf);
                pthread_mutex_lock(&x->x_mutex);
                x->x_sf.sf_fd = -1;
                x->x_sf.sf_data = NULL; /* closefn freed this */
            }
            if (x->x_requestcode == REQUEST_CLOSE)
                x->x_requestcode = REQUEST_NOTHING;
            sfread_cond_signal(&x->x_answercondition);
        }
        else if (x->x_requestcode == REQUEST_QUIT)
        {
            if (sf.sf_fd >= 0)
            {
                    /* use cached sf */
                pthread_mutex_unlock(&x->x_mutex);
                sf.sf_type->t_closefn(&sf);
                pthread_mutex_lock(&x->x_mutex);
                x->x_sf.sf_fd = -1;
                x->x_sf.sf_data = NULL; /* closefn freed this */
            }
            x->x_requestcode = REQUEST_NOTHING;
            sfread_cond_signal(&x->x_answercondition);
            break;
        }
        else
        {
#ifdef DEBUG_SOUNDFILE
            pute("13\n");
#endif
        }
    }
#ifdef DEBUG_SOUNDFILE
    pute("thread exit\n");
#endif
    pthread_mutex_unlock(&x->x_mutex);
    return 0;
}

/* ----- the object proper runs in the calling (parent) thread ----- */

static void readsf_tick(t_readsf *x);

static void *readsf_new(t_floatarg fnchannels, t_floatarg fbufsize)
{
    t_readsf *x;
    int nchannels = fnchannels, bufsize = fbufsize, i;
    char *buf;

    if (nchannels < 1)
        nchannels = 1;
    else if (nchannels > MAXSFCHANS)
        nchannels = MAXSFCHANS;
    if (bufsize <= 0) bufsize = DEFBUFPERCHAN * nchannels;
    else if (bufsize < MINBUFSIZE)
        bufsize = MINBUFSIZE;
    else if (bufsize > MAXBUFSIZE)
        bufsize = MAXBUFSIZE;
    buf = getbytes(bufsize);
    if (!buf) return 0;

    x = (t_readsf *)pd_new(readsf_class);

    for (i = 0; i < nchannels; i++)
        outlet_new(&x->x_obj, gensym("signal"));
    x->x_noutlets = nchannels;
    x->x_bangout = outlet_new(&x->x_obj, &s_bang);
    pthread_mutex_init(&x->x_mutex, 0);
    pthread_cond_init(&x->x_requestcondition, 0);
    pthread_cond_init(&x->x_answercondition, 0);
    x->x_vecsize = MAXVECSIZE;
    x->x_state = STATE_IDLE;
    x->x_clock = clock_new(x, (t_method)readsf_tick);
    x->x_canvas = canvas_getcurrent();
    soundfile_clear(&x->x_sf);
    x->x_sf.sf_bytespersample = 2;
    x->x_sf.sf_nchannels = 1;
    x->x_sf.sf_bytesperframe = 2;
    x->x_buf = buf;
    x->x_bufsize = bufsize;
    x->x_fifosize = x->x_fifohead = x->x_fifotail = x->x_requestcode = 0;
    pthread_create(&x->x_childthread, 0, readsf_child_main, x);
    return x;
}

static void readsf_tick(t_readsf *x)
{
    outlet_bang(x->x_bangout);
}

static t_int *readsf_perform(t_int *w)
{
    t_readsf *x = (t_readsf *)(w[1]);
    t_soundfile sf = {0};
    int vecsize = x->x_vecsize, noutlets = x->x_noutlets, i;
    size_t j;
    t_sample *fp;
    soundfile_copy(&sf, &x->x_sf);
    if (x->x_state == STATE_STREAM)
    {
        int wantbytes;
        pthread_mutex_lock(&x->x_mutex);
        wantbytes = vecsize * sf.sf_bytesperframe;
        while (!x->x_eof && x->x_fifohead >= x->x_fifotail &&
                x->x_fifohead < x->x_fifotail + wantbytes-1)
        {
#ifdef DEBUG_SOUNDFILE
            pute("wait...\n");
#endif
            sfread_cond_signal(&x->x_requestcondition);
            sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
                /* resync local variables -- bug fix thanks to Shahrokh */
            vecsize = x->x_vecsize;
            soundfile_copy(&sf, &x->x_sf);
            wantbytes = vecsize * sf.sf_bytesperframe;
#ifdef DEBUG_SOUNDFILE
            pute("done\n");
#endif
        }
        if (x->x_eof && x->x_fifohead >= x->x_fifotail &&
            x->x_fifohead < x->x_fifotail + wantbytes-1)
        {
            int xfersize;
            if (x->x_fileerror)
                object_readerror(x, "readsf", x->x_filename,
                    x->x_fileerror, &x->x_sf);
            clock_delay(x->x_clock, 0);
            x->x_state = STATE_IDLE;

                /* if there's a partial buffer left, copy it out */
            xfersize = (x->x_fifohead - x->x_fifotail + 1) /
                       sf.sf_bytesperframe;
            if (xfersize)
            {
                soundfile_xferin_sample(&sf, noutlets, x->x_outvec, 0,
                    (unsigned char *)(x->x_buf + x->x_fifotail), xfersize);
                vecsize -= xfersize;
            }
                /* then zero out the (rest of the) output */
            for (i = 0; i < noutlets; i++)
                for (j = vecsize, fp = x->x_outvec[i] + xfersize; j--;)
                    *fp++ = 0;

            sfread_cond_signal(&x->x_requestcondition);
            pthread_mutex_unlock(&x->x_mutex);
            return w + 2;
        }

        soundfile_xferin_sample(&sf, noutlets, x->x_outvec, 0,
            (unsigned char *)(x->x_buf + x->x_fifotail), vecsize);

        x->x_fifotail += wantbytes;
        if (x->x_fifotail >= x->x_fifosize)
            x->x_fifotail = 0;
        if ((--x->x_sigcountdown) <= 0)
        {
            sfread_cond_signal(&x->x_requestcondition);
            x->x_sigcountdown = x->x_sigperiod;
        }
        pthread_mutex_unlock(&x->x_mutex);
    }
    else
    {
        for (i = 0; i < noutlets; i++)
            for (j = vecsize, fp = x->x_outvec[i]; j--;)
                *fp++ = 0;
    }
    return w + 2;
}

    /** start making output.  If we're in the "startup" state change
        to the "running" state. */
static void readsf_start(t_readsf *x)
{
    if (x->x_state == STATE_STARTUP)
        x->x_state = STATE_STREAM;
    else pd_error(x, "readsf: start requested with no prior 'open'");
}

    /** LATER rethink whether you need the mutex just to set a variable? */
static void readsf_stop(t_readsf *x)
{
    pthread_mutex_lock(&x->x_mutex);
    x->x_state = STATE_IDLE;
    x->x_requestcode = REQUEST_CLOSE;
    sfread_cond_signal(&x->x_requestcondition);
    pthread_mutex_unlock(&x->x_mutex);
}

static void readsf_float(t_readsf *x, t_floatarg f)
{
    if (f != 0)
        readsf_start(x);
    else readsf_stop(x);
}

    /** open method.  Called as:
        open [flags] filename [onsetframes headersize channels bytespersample endianness]
        (if headersize is zero, header is taken to be automatically detected;
        thus, use the special "-1" to mean a truly headerless file.)
        if type implementation is set, pass this to open unless headersize is -1 */
static void readsf_open(t_readsf *x, t_symbol *s, int argc, t_atom *argv)
{
    t_symbol *filesym, *endian;
    t_float onsetframes, headersize, nchannels, bytespersample;
    t_soundfile_type *type = NULL;

    while (argc > 0 && argv->a_type == A_SYMBOL &&
        *argv->a_w.w_symbol->s_name == '-')
    {
        const char *flag = argv->a_w.w_symbol->s_name + 1;
        if (!strcmp(flag, "-"))
        {
            argc -= 1; argv += 1;
            break;
        }
        else
        {
                /* check for type by name */
            type = soundfile_firsttype();
            while (type) {
                if (!strcmp(flag, TYPENAME(type)))
                    break;
                type = soundfile_nexttype(type);
            }
            if (!type)
                goto usage; /* unknown flag */
            argc -= 1; argv += 1;
        }
    }
    filesym = atom_getsymbolarg(0, argc, argv);
    onsetframes = atom_getfloatarg(1, argc, argv);
    headersize = atom_getfloatarg(2, argc, argv);
    nchannels = atom_getfloatarg(3, argc, argv);
    bytespersample = atom_getfloatarg(4, argc, argv);
    endian = atom_getsymbolarg(5, argc, argv);
    if (!*filesym->s_name)
        return; /* no filename */

    pthread_mutex_lock(&x->x_mutex);
    soundfile_clearinfo(&x->x_sf);
    x->x_requestcode = REQUEST_OPEN;
    x->x_filename = filesym->s_name;
    x->x_fifotail = 0;
    x->x_fifohead = 0;
    if (*endian->s_name == 'b')
         x->x_sf.sf_bigendian = 1;
    else if (*endian->s_name == 'l')
         x->x_sf.sf_bigendian = 0;
    else if (*endian->s_name)
        pd_error(x, "endianness neither 'b' nor 'l'");
    else x->x_sf.sf_bigendian = sys_isbigendian();
    x->x_onsetframes = (onsetframes > 0 ? onsetframes : 0);
    x->x_sf.sf_headersize = (headersize > 0 ? headersize :
        (headersize == 0 ? -1 : 0));
    x->x_sf.sf_nchannels = (nchannels >= 1 ? nchannels : 1);
    x->x_sf.sf_bytespersample = (bytespersample > 2 ? bytespersample : 2);
    x->x_sf.sf_bytesperframe = x->x_sf.sf_nchannels * x->x_sf.sf_bytespersample;
    if (type && x->x_sf.sf_headersize >= 0)
    {
        post("readsf_open: '-%s' overridden by headersize", TYPENAME(type));
        x->x_sf.sf_type = NULL;
    }
    else
        x->x_sf.sf_type = type;
    x->x_eof = 0;
    x->x_fileerror = 0;
    x->x_state = STATE_STARTUP;
    sfread_cond_signal(&x->x_requestcondition);
    pthread_mutex_unlock(&x->x_mutex);
    return;
usage:
    pd_error(x, "usage: open [flags] filename [onset] [headersize]...");
    error("[nchannels] [bytespersample] [endian (b or l)]");
    post("flags: %s --", sf_typeargs);
}

static void readsf_dsp(t_readsf *x, t_signal **sp)
{
    int i, noutlets = x->x_noutlets;
    pthread_mutex_lock(&x->x_mutex);
    x->x_vecsize = sp[0]->s_n;
    x->x_sigperiod = x->x_fifosize / (x->x_sf.sf_bytesperframe * x->x_vecsize);
    for (i = 0; i < noutlets; i++)
        x->x_outvec[i] = sp[i]->s_vec;
    pthread_mutex_unlock(&x->x_mutex);
    dsp_add(readsf_perform, 1, x);
}

static void readsf_print(t_readsf *x)
{
    post("state %d", x->x_state);
    post("fifo head %d", x->x_fifohead);
    post("fifo tail %d", x->x_fifotail);
    post("fifo size %d", x->x_fifosize);
    post("fd %d", x->x_sf.sf_fd);
    post("eof %d", x->x_eof);
}

    /** request QUIT and wait for acknowledge */
static void readsf_free(t_readsf *x)
{
    void *threadrtn;
    pthread_mutex_lock(&x->x_mutex);
    x->x_requestcode = REQUEST_QUIT;
    sfread_cond_signal(&x->x_requestcondition);
    while (x->x_requestcode != REQUEST_NOTHING)
    {
        sfread_cond_signal(&x->x_requestcondition);
        sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
    }
    pthread_mutex_unlock(&x->x_mutex);
    if (pthread_join(x->x_childthread, &threadrtn))
        error("readsf_free: join failed");

    pthread_cond_destroy(&x->x_requestcondition);
    pthread_cond_destroy(&x->x_answercondition);
    pthread_mutex_destroy(&x->x_mutex);
    freebytes(x->x_buf, x->x_bufsize);
    clock_free(x->x_clock);
}

static void readsf_setup(void)
{
    readsf_class = class_new(gensym("readsf~"),
        (t_newmethod)readsf_new, (t_method)readsf_free,
        sizeof(t_readsf), 0, A_DEFFLOAT, A_DEFFLOAT, 0);
    class_addfloat(readsf_class, (t_method)readsf_float);
    class_addmethod(readsf_class, (t_method)readsf_start, gensym("start"), 0);
    class_addmethod(readsf_class, (t_method)readsf_stop, gensym("stop"), 0);
    class_addmethod(readsf_class, (t_method)readsf_dsp,
        gensym("dsp"), A_CANT, 0);
    class_addmethod(readsf_class, (t_method)readsf_open,
        gensym("open"), A_GIMME, 0);
    class_addmethod(readsf_class, (t_method)readsf_print, gensym("print"), 0);
}

/* ------------------------- writesf ------------------------- */

static t_class *writesf_class;

typedef t_readsf t_writesf; /* just re-use the structure */

/* ----- the child thread which performs file I/O ----- */

static void *writesf_child_main(void *zz)
{
    t_writesf *x = zz;
    t_soundfile sf = {0};
    soundfile_clear(&sf);
#ifdef DEBUG_SOUNDFILE
    pute("1\n");
#endif
    pthread_mutex_lock(&x->x_mutex);
    while (1)
    {
#ifdef DEBUG_SOUNDFILE
        pute("0\n");
#endif
        if (x->x_requestcode == REQUEST_NOTHING)
        {
#ifdef DEBUG_SOUNDFILE
            pute("wait 2\n");
#endif
            sfread_cond_signal(&x->x_answercondition);
            sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
            pute("3\n");
#endif
        }
        else if (x->x_requestcode == REQUEST_OPEN)
        {
            char boo[80];
            ssize_t byteswritten;
            size_t writebytes;

                /* copy file stuff out of the data structure so we can
                relinquish the mutex while we're in open_soundfile_via_path() */
            const char *filename = x->x_filename;
            t_canvas *canvas = x->x_canvas;
            soundfile_copy(&sf, &x->x_sf);

                /* alter the request code so that an ensuing "open" will get
                noticed. */
#ifdef DEBUG_SOUNDFILE
            pute("4\n");
#endif
            x->x_requestcode = REQUEST_BUSY;
            x->x_fileerror = 0;

                /* if there's already a file open, close it.  This
                should never happen since writesf_open() calls stop if
                needed and then waits until we're idle. */
            if (x->x_sf.sf_fd >= 0)
            {
                size_t frameswritten = x->x_frameswritten;

                pthread_mutex_unlock(&x->x_mutex);
                soundfile_finishwrite(x, filename, &sf,
                    SFMAXFRAMES, frameswritten);
                sf.sf_type->t_closefn(&sf);
                pthread_mutex_lock(&x->x_mutex);

                x->x_sf.sf_fd = -1;
                x->x_sf.sf_data = NULL; /* closefn freed this */

#ifdef DEBUG_SOUNDFILE
                {
                    char s[1000];
                    sprintf(s, "bug??? ditched %ld\n", frameswritten);
                    pute(s);
                }
#endif
                if (x->x_requestcode != REQUEST_BUSY)
                    continue;
            }

                /* open the soundfile with the mutex unlocked */
            pthread_mutex_unlock(&x->x_mutex);
            soundfile_copy(&sf, &x->x_sf);
            create_soundfile(canvas, filename, &sf, 0);
            pthread_mutex_lock(&x->x_mutex);

#ifdef DEBUG_SOUNDFILE
            pute("5\n");
#endif

            if (sf.sf_fd < 0)
            {
                x->x_sf.sf_fd = -1;
                x->x_sf.sf_data = NULL; /* create_soundfile freed this */
                x->x_eof = 1;
                x->x_fileerror = errno;
#ifdef DEBUG_SOUNDFILE
                pute("open failed\n");
                pute(filename);
#endif
                x->x_requestcode = REQUEST_NOTHING;
                continue;
            }
                /* check if another request has been made; if so, field it */
            if (x->x_requestcode != REQUEST_BUSY)
                continue;
#ifdef DEBUG_SOUNDFILE
            pute("6\n");
#endif
            soundfile_copy(&x->x_sf, &sf);
            x->x_fifotail = 0;
            x->x_frameswritten = 0;
                /* in a loop, wait for the fifo to have data and write it
                    to disk */
            while (x->x_requestcode == REQUEST_BUSY ||
                (x->x_requestcode == REQUEST_CLOSE &&
                    x->x_fifohead != x->x_fifotail))
            {
                int fifosize = x->x_fifosize, fifotail;
                char *buf = x->x_buf;
#ifdef DEBUG_SOUNDFILE
                pute("77\n");
#endif
                    /* if the head is < the tail, we can immediately write
                    from tail to end of fifo to disk; otherwise we hold off
                    writing until there are at least WRITESIZE bytes in the
                    buffer */
                if (x->x_fifohead < x->x_fifotail ||
                    x->x_fifohead >= x->x_fifotail + WRITESIZE
                    || (x->x_requestcode == REQUEST_CLOSE &&
                        x->x_fifohead != x->x_fifotail))
                {
                    writebytes = (x->x_fifohead < x->x_fifotail ?
                        fifosize : x->x_fifohead) - x->x_fifotail;
                    if (writebytes > READSIZE)
                        writebytes = READSIZE;
                }
                else
                {
#ifdef DEBUG_SOUNDFILE
                    pute("wait 7a...\n");
#endif
                    sfread_cond_signal(&x->x_answercondition);
#ifdef DEBUG_SOUNDFILE
                    pute("signaled\n");
#endif
                    sfread_cond_wait(&x->x_requestcondition,
                        &x->x_mutex);
#ifdef DEBUG_SOUNDFILE
                    pute("7a done\n");
#endif
                    continue;
                }
#ifdef DEBUG_SOUNDFILE
                pute("8\n");
#endif
                fifotail = x->x_fifotail;
                soundfile_copy(&sf, &x->x_sf);
                pthread_mutex_unlock(&x->x_mutex);
                byteswritten = sf.sf_type->t_writesamplesfn(&sf,
                    (unsigned char*)(buf + fifotail), writebytes);
                pthread_mutex_lock(&x->x_mutex);
                if (x->x_requestcode != REQUEST_BUSY &&
                    x->x_requestcode != REQUEST_CLOSE)
                        break;
                if (byteswritten < writebytes)
                {
#ifdef DEBUG_SOUNDFILE
                    pute("fileerror\n");
#endif
                    x->x_fileerror = errno;
                    break;
                }
                else
                {
                    x->x_fifotail += byteswritten;
                    if (x->x_fifotail == fifosize)
                        x->x_fifotail = 0;
                }
                x->x_frameswritten += byteswritten / x->x_sf.sf_bytesperframe;
#ifdef DEBUG_SOUNDFILE
                sprintf(boo, "after: head %d, tail %d written %ld\n",
                    x->x_fifohead, x->x_fifotail, x->x_frameswritten);
                pute(boo);
#endif
                    /* signal parent in case it's waiting for data */
                sfread_cond_signal(&x->x_answercondition);
            }
        }
        else if (x->x_requestcode == REQUEST_CLOSE ||
            x->x_requestcode == REQUEST_QUIT)
        {
            int quit = (x->x_requestcode == REQUEST_QUIT);
            if (x->x_sf.sf_fd >= 0)
            {
                const char *filename = x->x_filename;
                size_t frameswritten = x->x_frameswritten;
                soundfile_copy(&sf, &x->x_sf);
                pthread_mutex_unlock(&x->x_mutex);
                soundfile_finishwrite(x, filename, &sf,
                    SFMAXFRAMES, frameswritten);
                sf.sf_type->t_closefn(&sf);
                pthread_mutex_lock(&x->x_mutex);
                x->x_sf.sf_fd = -1;
                x->x_sf.sf_data = NULL; /* closefn freed this */
            }
            x->x_requestcode = REQUEST_NOTHING;
            sfread_cond_signal(&x->x_answercondition);
            if (quit)
                break;
        }
        else
        {
#ifdef DEBUG_SOUNDFILE
            pute("13\n");
#endif
        }
    }
#ifdef DEBUG_SOUNDFILE
    pute("thread exit\n");
#endif
    pthread_mutex_unlock(&x->x_mutex);
    return 0;
}

/* ----- the object proper runs in the calling (parent) thread ----- */

static void writesf_tick(t_writesf *x);

static void *writesf_new(t_floatarg fnchannels, t_floatarg fbufsize)
{
    t_writesf *x;
    int nchannels = fnchannels, bufsize = fbufsize, i;
    char *buf;

    if (nchannels < 1)
        nchannels = 1;
    else if (nchannels > MAXSFCHANS)
        nchannels = MAXSFCHANS;
    if (bufsize <= 0) bufsize = DEFBUFPERCHAN * nchannels;
    else if (bufsize < MINBUFSIZE)
        bufsize = MINBUFSIZE;
    else if (bufsize > MAXBUFSIZE)
        bufsize = MAXBUFSIZE;
    buf = getbytes(bufsize);
    if (!buf) return 0;

    x = (t_writesf *)pd_new(writesf_class);

    for (i = 1; i < nchannels; i++)
        inlet_new(&x->x_obj,  &x->x_obj.ob_pd, &s_signal, &s_signal);

    x->x_f = 0;
    pthread_mutex_init(&x->x_mutex, 0);
    pthread_cond_init(&x->x_requestcondition, 0);
    pthread_cond_init(&x->x_answercondition, 0);
    x->x_vecsize = MAXVECSIZE;
    x->x_insamplerate = 0;
    x->x_state = STATE_IDLE;
    x->x_clock = 0;     /* no callback needed here */
    x->x_canvas = canvas_getcurrent();
    soundfile_clear(&x->x_sf);
    x->x_sf.sf_nchannels = nchannels;
    x->x_sf.sf_bytespersample = 2;
    x->x_sf.sf_bytesperframe = nchannels * 2;
    x->x_buf = buf;
    x->x_bufsize = bufsize;
    x->x_fifosize = x->x_fifohead = x->x_fifotail = x->x_requestcode = 0;
    pthread_create(&x->x_childthread, 0, writesf_child_main, x);
    return x;
}

static t_int *writesf_perform(t_int *w)
{
    t_writesf *x = (t_writesf *)(w[1]);
    t_soundfile sf = {0};
    int vecsize = x->x_vecsize;
    soundfile_copy(&sf, &x->x_sf);
    if (x->x_state == STATE_STREAM)
    {
        int roominfifo;
        size_t wantbytes;
        pthread_mutex_lock(&x->x_mutex);
        wantbytes = vecsize * sf.sf_bytesperframe;
        roominfifo = x->x_fifotail - x->x_fifohead;
        if (roominfifo <= 0)
            roominfifo += x->x_fifosize;
        while (roominfifo < wantbytes + 1)
        {
            fprintf(stderr, "writesf waiting for disk write..\n");
            fprintf(stderr, "(head %d, tail %d, room %d, want %ld)\n",
                x->x_fifohead, x->x_fifotail, roominfifo, wantbytes);
            sfread_cond_signal(&x->x_requestcondition);
            sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
            fprintf(stderr, "... done waiting.\n");
            roominfifo = x->x_fifotail - x->x_fifohead;
            if (roominfifo <= 0)
                roominfifo += x->x_fifosize;
        }

        soundfile_xferout_sample(&sf, x->x_outvec,
            (unsigned char *)(x->x_buf + x->x_fifohead), vecsize, 0, 1.);

        x->x_fifohead += wantbytes;
        if (x->x_fifohead >= x->x_fifosize)
            x->x_fifohead = 0;
        if ((--x->x_sigcountdown) <= 0)
        {
#ifdef DEBUG_SOUNDFILE
            pute("signal 1\n");
#endif
            sfread_cond_signal(&x->x_requestcondition);
            x->x_sigcountdown = x->x_sigperiod;
        }
        pthread_mutex_unlock(&x->x_mutex);
    }
    return w + 2;
}

    /** start making output.  If we're in the "startup" state change
        to the "running" state. */
static void writesf_start(t_writesf *x)
{
    if (x->x_state == STATE_STARTUP)
        x->x_state = STATE_STREAM;
    else
        pd_error(x, "writesf: start requested with no prior 'open'");
}

    /** LATER rethink whether you need the mutex just to set a variable? */
static void writesf_stop(t_writesf *x)
{
    pthread_mutex_lock(&x->x_mutex);
    x->x_state = STATE_IDLE;
    x->x_requestcode = REQUEST_CLOSE;
#ifdef DEBUG_SOUNDFILE
    pute("signal 2\n");
#endif
    sfread_cond_signal(&x->x_requestcondition);
    pthread_mutex_unlock(&x->x_mutex);
}

    /** open method.  Called as: open [flags] filename with args as in
        soundfiler_parsewriteargs(). */
static void writesf_open(t_writesf *x, t_symbol *s, int argc, t_atom *argv)
{
    t_soundfiler_writeargs wa = {0};
    if (x->x_state != STATE_IDLE)
        writesf_stop(x);
    if (soundfiler_parsewriteargs(x, &argc, &argv, &wa))
    {
        pd_error(x, "usage: open [flags] filename...");
        post("flags: -bytes <n> %s ...", sf_typeargs);
        post("-big -little -rate <n> --");
        return;
    }
    if (wa.wa_normalize || wa.wa_onsetframes || (wa.wa_nframes != SFMAXFRAMES))
        pd_error(x, "normalize/onset/nframes argument to writesf~ ignored");
    if (argc)
        pd_error(x, "extra argument(s) to writesf~ ignored");
    pthread_mutex_lock(&x->x_mutex);
    while (x->x_requestcode != REQUEST_NOTHING)
    {
        sfread_cond_signal(&x->x_requestcondition);
        sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
    }
    x->x_filename = wa.wa_filesym->s_name;
    x->x_sf.sf_type = wa.wa_type;
    if (wa.wa_samplerate > 0)
        x->x_sf.sf_samplerate = wa.wa_samplerate;
    else if (x->x_insamplerate > 0)
        x->x_sf.sf_samplerate = x->x_insamplerate;
    else x->x_sf.sf_samplerate = sys_getsr();
    x->x_sf.sf_bytespersample =
        (wa.wa_bytespersample > 2 ? wa.wa_bytespersample : 2);
    x->x_sf.sf_bigendian = wa.wa_bigendian;
    x->x_sf.sf_bytesperframe = x->x_sf.sf_nchannels * x->x_sf.sf_bytespersample;
    x->x_frameswritten = 0;
    x->x_requestcode = REQUEST_OPEN;
    x->x_fifotail = 0;
    x->x_fifohead = 0;
    x->x_eof = 0;
    x->x_fileerror = 0;
    x->x_state = STATE_STARTUP;
        /* set fifosize from bufsize.  fifosize must be a
        multiple of the number of bytes eaten for each DSP
        tick.  */
    x->x_fifosize = x->x_bufsize - (x->x_bufsize %
        (x->x_sf.sf_bytesperframe * MAXVECSIZE));
        /* arrange for the "request" condition to be signaled 16
            times per buffer */
    x->x_sigcountdown = x->x_sigperiod = (x->x_fifosize /
            (16 * (x->x_sf.sf_bytesperframe * x->x_vecsize)));
    sfread_cond_signal(&x->x_requestcondition);
    pthread_mutex_unlock(&x->x_mutex);
}

    /** write metadata method.  Called as: meta args... and passed to the
        type implementation */
static void writesf_meta(t_writesf *x, t_symbol *s, int argc, t_atom *argv)
{
    if (x->x_state == STATE_IDLE)
    {
        pd_error(x, "writesf: meta with no prior 'open'");
        return;
    }
    if (x->x_state == STATE_STREAM)
    {
        pd_error(x, "writesf: meta after 'start'");
        return;
    }
    if (!x->x_sf.sf_type)
    {
        /* this shouldn't happen... */
        pd_error(x, "writesf: meta ignored, unknown type implementation");
        return;
    }
    if (!x->x_sf.sf_type->t_writemetafn)
    {
        pd_error(x, "writesf: %s does not support writing metadata",
            TYPENAME(x->x_sf.sf_type));
        return;
    }
    if (!x->x_sf.sf_type->t_writemetafn(&x->x_sf, argc, argv))
    {
        pd_error(x, "writesf: writing %s metadata failed",
            TYPENAME(x->x_sf.sf_type));
    }
}

static void writesf_dsp(t_writesf *x, t_signal **sp)
{
    int i, ninlets = x->x_sf.sf_nchannels;
    pthread_mutex_lock(&x->x_mutex);
    x->x_vecsize = sp[0]->s_n;
    x->x_sigperiod = (x->x_fifosize /
            (16 * x->x_sf.sf_bytesperframe * x->x_vecsize));
    for (i = 0; i < ninlets; i++)
        x->x_outvec[i] = sp[i]->s_vec;
    x->x_insamplerate = sp[0]->s_sr;
    pthread_mutex_unlock(&x->x_mutex);
    dsp_add(writesf_perform, 1, x);
}

static void writesf_print(t_writesf *x)
{
    post("state %d", x->x_state);
    post("fifo head %d", x->x_fifohead);
    post("fifo tail %d", x->x_fifotail);
    post("fifo size %d", x->x_fifosize);
    post("fd %d", x->x_sf.sf_fd);
    post("eof %d", x->x_eof);
}

    /** request QUIT and wait for acknowledge */
static void writesf_free(t_writesf *x)
{
    void *threadrtn;
    pthread_mutex_lock(&x->x_mutex);
    x->x_requestcode = REQUEST_QUIT;
#ifdef DEBUG_SOUNDFILE
    post("stopping writesf thread...");
#endif
    sfread_cond_signal(&x->x_requestcondition);
    while (x->x_requestcode != REQUEST_NOTHING)
    {
#ifdef DEBUG_SOUNDFILE
        post("signaling...");
#endif
        sfread_cond_signal(&x->x_requestcondition);
        sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
    }
    pthread_mutex_unlock(&x->x_mutex);
    if (pthread_join(x->x_childthread, &threadrtn))
        error("writesf_free: join failed");
#ifdef DEBUG_SOUNDFILE
    post("... done.");
#endif

    pthread_cond_destroy(&x->x_requestcondition);
    pthread_cond_destroy(&x->x_answercondition);
    pthread_mutex_destroy(&x->x_mutex);
    freebytes(x->x_buf, x->x_bufsize);
}

static void writesf_setup(void)
{
    writesf_class = class_new(gensym("writesf~"),
        (t_newmethod)writesf_new, (t_method)writesf_free,
        sizeof(t_writesf), 0, A_DEFFLOAT, A_DEFFLOAT, 0);
    class_addmethod(writesf_class, (t_method)writesf_start, gensym("start"), 0);
    class_addmethod(writesf_class, (t_method)writesf_stop, gensym("stop"), 0);
    class_addmethod(writesf_class, (t_method)writesf_dsp,
        gensym("dsp"), A_CANT, 0);
    class_addmethod(writesf_class, (t_method)writesf_open,
        gensym("open"), A_GIMME, 0);
    class_addmethod(writesf_class, (t_method)writesf_meta,
        gensym("meta"), A_GIMME, 0);
    class_addmethod(writesf_class, (t_method)writesf_print, gensym("print"), 0);
    CLASS_MAINSIGNALIN(writesf_class, t_writesf, x_f);
}

/* ------------------------- global setup routine ------------------------ */

void d_soundfile_setup(void)
{
    soundfile_type_setup();
    soundfiler_setup();
    readsf_setup();
    writesf_setup();
}
