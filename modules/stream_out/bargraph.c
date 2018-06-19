/*****************************************************************************
 * bargraph.c: bargraph stream output module
 *****************************************************************************
 * Copyright (C) 2018 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Pierre Lamot
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <limits.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_modules.h>
#include <vlc_codec.h>
#include <vlc_aout.h>
#include <vlc_block.h>
#include <vlc_sout.h>
#include <vlc_fs.h>
#include <vlc_iso_lang.h>
#include "../../src/text/iso-639_def.h"
#include <assert.h>

/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/
static int      Open    ( vlc_object_t * );
static void     Close   ( vlc_object_t * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define DST_PREFIX_TEXT N_("Destination prefix")
#define DST_PREFIX_LONGTEXT N_( \
    "Prefix of the destination file automatically generated" )

#define SOUT_CFG_PREFIX "sout-bargraph-"

vlc_module_begin ()
    set_description( N_("Bargraph stream output") )
    set_capability( "sout stream", 0 )
    add_shortcut( "bargraph" )
    set_shortname( N_("Bargraph") )

    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_STREAM )

    set_callbacks( Open, Close )
vlc_module_end ()

/* */
static const char *const ppsz_sout_options[] = {
    NULL
};

/* */
static sout_stream_id_sys_t *Add( sout_stream_t *, const es_format_t * );
static void              Del ( sout_stream_t *, sout_stream_id_sys_t * );
static int               Send( sout_stream_t *, sout_stream_id_sys_t *, block_t* );

static char *LanguageGetCode( const char *psz_lang )
{
    const iso639_lang_t *pl;

    if( psz_lang == NULL || *psz_lang == '\0' )
        return strdup("??");

    for( pl = p_languages; pl->psz_eng_name != NULL; pl++ )
    {
        if( !strcasecmp( pl->psz_eng_name, psz_lang ) ||
            !strcasecmp( pl->psz_iso639_1, psz_lang ) ||
            !strcasecmp( pl->psz_iso639_2T, psz_lang ) ||
            !strcasecmp( pl->psz_iso639_2B, psz_lang ) )
            return strdup( pl->psz_iso639_1 );
    }

    return strdup("??");
}

static char *LanguageGetName( const char *psz_code )
{
    const iso639_lang_t *pl;

    if( psz_code == NULL || !strcmp( psz_code, "und" ) )
    {
        return strdup( "" );
    }

    if( strlen( psz_code ) == 2 )
    {
        pl = GetLang_1( psz_code );
    }
    else if( strlen( psz_code ) == 3 )
    {
        pl = GetLang_2B( psz_code );
        if( !strcmp( pl->psz_iso639_1, "??" ) )
        {
            pl = GetLang_2T( psz_code );
        }
    }
    else
    {
        char *lang = LanguageGetCode( psz_code );
        pl = GetLang_1( lang );
        free( lang );
    }

    if( !strcmp( pl->psz_iso639_1, "??" ) )
    {
       return strdup( psz_code );
    }
    else
    {
        return strdup( vlc_gettext(pl->psz_eng_name) );
    }
}

typedef struct {
    float channels_peaks[AOUT_CHAN_MAX];
} peak_data_t;

typedef struct  {
    char* psz_stream_name;
    int i_stream_id;
    int i_nb_channels;
    vlc_fifo_t* p_fifo;
} bargraph_data_t;

typedef struct {
    vlc_mutex_t mutex;
    int i_count_channels;
    int i_streams;
    int i_refcount;
    bargraph_data_t** p_streams;
} shared_bargraph_data_t;

/* */
struct sout_stream_id_sys_t
{
    es_format_t fmt;

    sout_stream_id_sys_t *id;
    decoder_t       *p_decoder;

    bargraph_data_t* p_data;
    sout_stream_sys_t* p_sys;
};

struct sout_stream_sys_t
{
    int              i_id;
    sout_stream_id_sys_t **id;

    shared_bargraph_data_t* shared_data;
};


static int bargraph_data_cmp(const void* p_a, const void* p_b)
{
    bargraph_data_t** p_bd_a = (bargraph_data_t**) p_a;
    bargraph_data_t** p_bd_b = (bargraph_data_t**) p_b;
    return (*p_bd_a)->i_stream_id - (*p_bd_b)->i_stream_id;
}

static void shared_bargraph_data_sort_streams(shared_bargraph_data_t* p_data)
{
    qsort(p_data->p_streams, p_data->i_streams, sizeof(*p_data->p_streams), bargraph_data_cmp);
}

static shared_bargraph_data_t* shared_bargraph_data_new()
{
    shared_bargraph_data_t* p_shared_data = (shared_bargraph_data_t*) calloc(1, sizeof(shared_bargraph_data_t));
    vlc_mutex_init(&p_shared_data->mutex);
    p_shared_data->i_refcount = 1;
    TAB_INIT( p_shared_data->i_streams, p_shared_data->p_streams );
    return p_shared_data;
}

static void shared_bargraph_data_unref( shared_bargraph_data_t* p_shared_data )
{
    if (!p_shared_data)
        return;
    vlc_mutex_lock(&p_shared_data->mutex);
    assert(p_shared_data->i_refcount >= 1);
    p_shared_data->i_refcount--;
    if (p_shared_data->i_refcount == 0)
    {
        TAB_CLEAN( p_shared_data->i_streams, p_shared_data->p_streams );
        vlc_mutex_unlock(&p_shared_data->mutex);
        vlc_mutex_destroy(&p_shared_data->mutex);
        free( p_shared_data );
    }
    else
        vlc_mutex_unlock(&p_shared_data->mutex);
}

static bargraph_data_t* shared_bargraph_data_add_stream(shared_bargraph_data_t* p_data, const es_format_t * p_fmt)
{
    if (!p_data)
        return NULL;
    bargraph_data_t* p_bargraph_data = calloc(1, sizeof(bargraph_data_t));
    p_bargraph_data->p_fifo = block_FifoNew();
    p_bargraph_data->i_stream_id = p_fmt->i_id;
    p_bargraph_data->i_nb_channels = p_fmt->audio.i_channels;
    if (p_fmt->psz_language)
    {
        char* psz_lang = LanguageGetName( p_fmt->psz_language );
        asprintf(&p_bargraph_data->psz_stream_name, "%i [%s]", p_fmt->i_id, p_fmt->psz_language );
        free( psz_lang );
    }
    else
        asprintf(&p_bargraph_data->psz_stream_name, "%i", p_fmt->i_id);

    vlc_mutex_lock(&p_data->mutex);
    TAB_APPEND( p_data->i_streams, p_data->p_streams, p_bargraph_data );
    shared_bargraph_data_sort_streams(p_data);
    p_data->i_count_channels += p_bargraph_data->i_nb_channels;
    vlc_mutex_unlock(&p_data->mutex);
    return p_bargraph_data;
}

static void shared_bargraph_data_del_stream(shared_bargraph_data_t* p_data, bargraph_data_t* p_bargraph_data )
{
    vlc_mutex_lock(&p_data->mutex);
    TAB_REMOVE( p_data->i_streams, p_data->p_streams, p_bargraph_data );
    shared_bargraph_data_sort_streams(p_data);
    p_data->i_count_channels -= p_bargraph_data->i_nb_channels;
    vlc_mutex_unlock(&p_data->mutex);
    if (p_bargraph_data->psz_stream_name)
        free(p_bargraph_data->psz_stream_name);
    block_FifoRelease(p_bargraph_data->p_fifo);
    free( p_bargraph_data );
}


/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_stream_t *p_stream = (sout_stream_t*)p_this;
    sout_stream_sys_t *p_sys;

    p_stream->pf_add    = Add;
    p_stream->pf_del    = Del;
    p_stream->pf_send   = Send;

    p_stream->p_sys = p_sys = calloc( 1, sizeof(*p_sys) );
    if( !p_sys )
        return VLC_ENOMEM;


    config_ChainParse( p_stream, SOUT_CFG_PREFIX, ppsz_sout_options, p_stream->p_cfg );

    p_sys->shared_data = shared_bargraph_data_new();
    TAB_INIT( p_sys->i_id, p_sys->id );

    var_Create(p_this->obj.libvlc, "audiobargraph_v-alarm", VLC_VAR_BOOL);
    var_Create(p_this->obj.libvlc, "audiobargraph_v-i_values", VLC_VAR_ADDRESS);


    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    sout_stream_t *p_stream = (sout_stream_t*)p_this;
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    msg_Warn( p_stream, "Close bargraph stream" );

    TAB_CLEAN( p_sys->i_id, p_sys->id );
    var_Destroy(p_this->obj.libvlc, "audiobargraph_v-alarm");
    var_SetAddress(p_this->obj.libvlc, "audiobargraph_v-i_values", NULL);
    var_Destroy(p_this->obj.libvlc, "audiobargraph_v-i_values");
    shared_bargraph_data_unref( p_sys->shared_data );
    free( p_sys );
}

static inline int audio_update_format( decoder_t *p_dec )
{
    assert(p_dec->fmt_in.i_cat == AUDIO_ES);
    p_dec->fmt_out.audio.i_format = p_dec->fmt_out.i_codec;
    aout_FormatPrepare( &p_dec->fmt_out.audio );
    return ( p_dec->fmt_out.audio.i_bitspersample > 0 ) ? 0 : -1;
}

static int  decoder_queue_audio( decoder_t * p_dec, block_t * block_in)
{
    sout_stream_id_sys_t *id = p_dec->p_queue_ctx;

    int nbChannels = aout_FormatNbChannels(&p_dec->fmt_out.audio);
    float f_value[AOUT_CHAN_MAX];
    memset(f_value, 0, sizeof(f_value));

#define MAX_CHANNELS_AS_TYPE(type, i_value)                 \
    do {                                                    \
        memset(i_value, 0, sizeof(i_value));                \
        type *p_sample = (type *)block_in->p_buffer;            \
        for (unsigned i = 0; i < block_in->i_nb_samples; i++)   \
            for (int j = 0; j < nbChannels; j++) {          \
                type ch = *p_sample++;                      \
                if (ch > i_value[j])                        \
                    i_value[j] = ch;                        \
            }                                               \
    } while(0)

    if (p_dec->fmt_out.i_codec == VLC_CODEC_F32L)
    {
        MAX_CHANNELS_AS_TYPE(float, f_value);
    }
    else if (p_dec->fmt_out.i_codec == VLC_CODEC_F64L)
    {
        double d_value[AOUT_CHAN_MAX];
        MAX_CHANNELS_AS_TYPE(double, d_value);
        for (int i = 0; i < nbChannels; i++)
            f_value[i] = (float)d_value[i];
    }
    else if (p_dec->fmt_out.i_codec == VLC_CODEC_S32N)
    {
        int32_t d_value[AOUT_CHAN_MAX];
        MAX_CHANNELS_AS_TYPE(int32_t, d_value);
        for (int i = 0; i < nbChannels; i++)
            f_value[i] = (float)d_value[i] / 2147483648.f;
    }
    else if (p_dec->fmt_out.i_codec == VLC_CODEC_S16N)
    {
        int16_t i_value[AOUT_CHAN_MAX];
        MAX_CHANNELS_AS_TYPE(int16_t, i_value);
        for (int i = 0; i < nbChannels; i++)
            f_value[i] = (float)i_value[i] / 32768.f;
    }
    else if (p_dec->fmt_out.i_codec == VLC_CODEC_U8)
    {
        uint8_t i_value[AOUT_CHAN_MAX];
        MAX_CHANNELS_AS_TYPE(uint8_t, i_value);
        for (int i = 0; i < nbChannels; i++)
            f_value[i] = (float)(i_value[i] - 128) / 128.f;
    }
    else
    {
        msg_Err(p_dec, "unsupported audio format %.4s", (char*)&p_dec->fmt_out.i_codec);
    }

#undef SUM_CHANNELS_AS_SIGNED_TYPE

    //send the values
    shared_bargraph_data_t* shared_data = id->p_sys->shared_data;
    vlc_mutex_lock(&shared_data->mutex);

    bargraph_data_t* data = id->p_data;
    block_t* block_out = block_Alloc(sizeof(peak_data_t));
    block_CopyProperties(block_out, block_in);
    memcpy(&((peak_data_t*)block_out->p_buffer)->channels_peaks, f_value, nbChannels * sizeof(float));
    vlc_fifo_Lock(data->p_fifo);
    //don't leak is nobody is consuming the fifo
    if ( vlc_fifo_GetCount(data->p_fifo) > 100 )
    {
        msg_Dbg(p_dec, "Drop peak data");
        block_t* block_tmp = vlc_fifo_DequeueUnlocked(data->p_fifo);
        block_Release(block_tmp);
    }
    vlc_fifo_QueueUnlocked(data->p_fifo, block_out);
    vlc_fifo_Unlock(data->p_fifo);
    vlc_mutex_unlock(&shared_data->mutex);
    var_SetAddress(p_dec->obj.libvlc, "audiobargraph_v-i_values", shared_data);


    block_Release( block_in );
    return 0;
}

/*****************************************************************************
 *
 *****************************************************************************/
static sout_stream_id_sys_t *Add( sout_stream_t *p_stream, const es_format_t *p_fmt )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_sys_t *id;

    if ( p_fmt->i_cat == AUDIO_ES )
    {
        msg_Warn( p_stream, "add audio steam %i", p_fmt->i_id );

        id = calloc( 1, sizeof(*id) );
        if( !id )
            return NULL;
        id->p_sys = p_sys;

        /* initialize decoder */
        id->p_decoder = vlc_object_create( p_stream, sizeof( decoder_t ) );
        if( !id->p_decoder )
            return NULL;
        id->p_decoder->p_module = NULL;

        //FIXME VLC_CODEC_F32L is not respected
        es_format_Init( &id->p_decoder->fmt_out, AUDIO_ES, VLC_CODEC_F32L );
        es_format_Copy( &id->p_decoder->fmt_in, p_fmt );
        id->p_decoder->b_frame_drop_allowed = false;

        id->p_decoder->pf_decode = NULL;
        id->p_decoder->pf_queue_audio = decoder_queue_audio;
        id->p_decoder->p_queue_ctx = id;
        id->p_decoder->pf_aout_format_update = audio_update_format;

        id->p_decoder->p_module =
            module_need( id->p_decoder, "audio decoder", "$codec", false );
        if( !id->p_decoder->p_module )
        {
            msg_Err( p_stream, "cannot find audio decoder" );
            goto error;
        }
    }
    else if ( p_fmt->i_cat == VIDEO_ES )
    {
        msg_Warn( p_stream, "add video steam %i (discard)", p_fmt->i_id );
        return NULL;
    }
    else
    {
        msg_Err( p_stream, "add other steam %i (discard)", p_fmt->i_id );
        return NULL;
    }


    id->p_data = shared_bargraph_data_add_stream( p_sys->shared_data, p_fmt );
    TAB_APPEND( p_sys->i_id, p_sys->id, id );

    return id;

error:
    vlc_object_release( id->p_decoder );
    free(id);
    return NULL;
}

static void Del( sout_stream_t *p_stream, sout_stream_id_sys_t *id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    if (id == NULL)
        return;

    assert( !id->id );
    msg_Warn    ( p_stream, "del audio steam %i", id->p_data->i_stream_id );

    if ( id->p_data )
        shared_bargraph_data_del_stream( p_sys->shared_data, id->p_data );

    /* Close decoder */
    if( id->p_decoder->p_module )
        module_unneed( id->p_decoder, id->p_decoder->p_module );
    es_format_Clean( &id->p_decoder->fmt_in );
    es_format_Clean( &id->p_decoder->fmt_out );
    vlc_object_release( id->p_decoder );

    TAB_REMOVE( p_sys->i_id, p_sys->id, id );


    free( id );
}

static int Send( sout_stream_t* p_stream, sout_stream_id_sys_t *id,
                 block_t *p_buffer )
{
    VLC_UNUSED(p_stream);
    //sout_stream_sys_t *p_sys = p_stream->p_sys;

    switch( id->p_decoder->fmt_in.i_cat )
    {
    case AUDIO_ES:
    {
        int ret = id->p_decoder->pf_decode( id->p_decoder, p_buffer );
        if( ret != VLCDEC_SUCCESS )
            return VLC_EGENERIC;
        break;
    }
    default:
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}
