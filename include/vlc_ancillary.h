/*****************************************************************************
 * vlc_ancillary.h: Ancillary data declarations
 *****************************************************************************
 * Copyright (C) 2016 VLC authors and VideoLAN
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
#ifndef VLC_ANCILLARY_H
#define VLC_ANCILLARY_H

typedef enum
{
    ANCILLARY_UNDEFINED       = 0,
    ANCILLARY_CLOSED_CAPTIONS = 1 << 0,
    ANCILLARY_AFD             = 1 << 1,
    ANCILLARY_BAR             = 1 << 2,

} vlc_ancillary_type_t;

typedef struct vlc_ancillary_t vlc_ancillary_t;

struct vlc_ancillary_t
{
    vlc_ancillary_type_t type;
    vlc_ancillary_t *p_next;

    union
    {
        struct
        {
            uint8_t *p_data;
            size_t i_data;
        } undefined, cc;
        struct
        {
            int8_t val;
        } afd;
        struct
        {
            int16_t top;
            int16_t bottom;
            int16_t left;
            int16_t right;
        } bar;
    };
};

static inline vlc_ancillary_t *
vlc_ancillary_New( vlc_ancillary_type_t type )
{
    vlc_ancillary_t *p_anc = (vlc_ancillary_t *) malloc(sizeof(*p_anc));
    if(likely(p_anc))
    {
        p_anc->p_next = NULL;
        p_anc->type = type;
        memset( &p_anc->bar, 0, sizeof(p_anc->bar) );
    }
    return p_anc;
}

static inline void vlc_ancillary_Delete( vlc_ancillary_t *p_anc )
{
    if( p_anc->type == ANCILLARY_UNDEFINED )
        free( p_anc->undefined.p_data );
    if( p_anc->type == ANCILLARY_CLOSED_CAPTIONS )
        free( p_anc->cc.p_data );
    free( p_anc );
}

static inline void vlc_ancillary_StorageAppend( vlc_ancillary_t **pp_stor,
                                                vlc_ancillary_t *p_anc )
{
    p_anc->p_next = *pp_stor;
    *pp_stor = p_anc;
    for( vlc_ancillary_type_t type = p_anc->type; p_anc != NULL; )
    {
        if( p_anc->p_next && p_anc->p_next->type == type )
        {
            vlc_ancillary_t *p_dup = p_anc->p_next;
            p_anc->p_next = p_dup->p_next;
            p_dup->p_next = NULL;
            vlc_ancillary_Delete( p_dup );
        }
        else
            p_anc = p_anc->p_next;
    }
}

static inline void vlc_ancillary_StorageDelete( vlc_ancillary_t *p_stor )
{
    while( p_stor )
    {
        vlc_ancillary_t *p_next = p_stor->p_next;
        vlc_ancillary_Delete( p_stor );
        p_stor = p_next;
    }
}

#endif
