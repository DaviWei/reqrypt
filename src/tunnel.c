/*
 * tunnel.c
 * (C) 2014, all rights reserved,
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cktp.h"
#include "cktp_client.h"
#include "cktp_url.h"
#include "http_server.h"
#include "log.h"
#include "misc.h"
#include "random.h"
#include "socket.h"
#include "thread.h"
#include "tunnel.h"

#define TUNNELS_BAK_FILENAME        TUNNELS_FILENAME ".bak"
#define TUNNELS_TMP_FILENAME        TUNNELS_FILENAME ".tmp"

#define MAX_ACTIVE_TUNNELS          8

typedef uint8_t state_t;

#define TUNNEL_STATE_CLOSED         0   // Tunnel is closed.
#define TUNNEL_STATE_OPENING        1   // Tunnel is being opened.
#define TUNNEL_STATE_OPEN           2   // Tunnel is open and ready for use.
#define TUNNEL_STATE_DEAD           3   // Tunnel is closed and not for use.
#define TUNNEL_STATE_CLOSING        4   // Tunnel is scheduled to be closed.
#define TUNNEL_STATE_DELETING       5   // Tunnel is scheduled to be deleted.

#define TUNNEL_INIT_AGE             16

#define TUNNEL_NO_TIMEOUT           0

struct tunnel_s
{
    cktp_tunnel_t    tunnel;            // Underlying CKTP tunnel
    state_t          state;             // Tunnel's state
    bool             reconnect;         // True if we are reconnecting
    uint16_t         id;                // Tunnel's ID
    uint8_t          age;               // Tunnel's age
    double           weight;            // Tunnel's weight
    char             url[CKTP_MAX_URL_LENGTH+1];
};

/*
 * Implementation of a set of tunnels.
 */
struct tunnel_set_s
{
    tunnel_t *tunnels;                  // Array of tunnels.
    size_t    size;                     // Array allocated size.
    size_t    length;                   // Array length
};
typedef struct tunnel_set_s *tunnel_set_t;
#define TUNNEL_SET_INIT         {NULL, 0, 0}
#define TUNNEL_SET_INIT_SIZE    16

/*
 * Tunnel history.
 */
struct tunnel_history_s
{
    uint32_t hash;
    uint16_t id;
};

#define TUNNEL_HISTORY_SIZE     1024

/*
 * Tunnel sets.
 */
static mutex_t tunnels_lock;
static struct tunnel_set_s tunnels_cache = TUNNEL_SET_INIT;
static struct tunnel_set_s tunnels_active = TUNNEL_SET_INIT;
static random_state_t rng = NULL;

/*
 * Prototypes.
 */
static bool tunnel_html(http_buffer_t buff, tunnel_set_t tunnel_set);
static void tunnel_set_insert(tunnel_set_t tunnel_set, tunnel_t tunnel);
static tunnel_t tunnel_set_replace(tunnel_set_t tunnel_set, tunnel_t tunnel);
static tunnel_t tunnel_set_delete(tunnel_set_t tunnel_set, const char *url);
static int tunnel_set_lookup(tunnel_set_t tunnel_set, const char *url);
static tunnel_t tunnel_create(const char *url, uint8_t age);
static void tunnel_free(tunnel_t tunnel);
static void *tunnel_activate_manager(void *unused);
static void *tunnel_activate(void *tunnel_ptr);
static bool tunnel_try_activate(tunnel_t tunnel);
static tunnel_t tunnel_get(uint64_t hash, unsigned repeat);
static void *tunnel_reconnect_manager(void *unused);
static void *tunnel_reconnect(void *tunnel_ptr);

/*
 * Print all tunnels as HTML.
 */
static bool tunnel_html(http_buffer_t buff, tunnel_set_t tunnel_set)
{
    thread_lock(&tunnels_lock);
    for (size_t i = 0; i < tunnel_set->length; i++)
    {
        http_buffer_puts(buff, "<option value=\"");
        http_buffer_puts(buff, tunnel_set->tunnels[i]->url);
        http_buffer_puts(buff, "\">");
        http_buffer_puts(buff, tunnel_set->tunnels[i]->url);
        http_buffer_puts(buff, "</option>\n");
    }
    thread_unlock(&tunnels_lock);
    return true;
}

/*
 * Print active tunnels as HTML.
 */
bool tunnel_active_html(http_buffer_t buff)
{
    return tunnel_html(buff, &tunnels_active);
}

/*
 * Print all tunnels as HTML.
 */
bool tunnel_all_html(http_buffer_t buff)
{
    return tunnel_html(buff, &tunnels_cache);
}

/*
 * Add a tunnel to a tunnel_set_s.
 */
static void tunnel_set_insert(tunnel_set_t tunnel_set, tunnel_t tunnel)
{
    if (tunnel_set->length >= tunnel_set->size)
    {
        tunnel_set->size = (tunnel_set->size == 0? TUNNEL_SET_INIT_SIZE:
            tunnel_set->size*2);
        size_t alloc_size = tunnel_set->size * sizeof(tunnel_t);
        tunnel_set->tunnels = (tunnel_t *)realloc(tunnel_set->tunnels,
            alloc_size);
        if (tunnel_set->tunnels == NULL)
        {
            error("unable to reallocate " SIZE_T_FMT " bytes for tunnel set",
                alloc_size);
        }
    }
    tunnel_set->tunnels[tunnel_set->length++] = tunnel;
}

/*
 * Replace a tunnel in a tunnel_set_s.
 */
static tunnel_t tunnel_set_replace(tunnel_set_t tunnel_set, tunnel_t tunnel)
{
    for (size_t i = 0; i < tunnel_set->length; i++)
    {
        tunnel_t tunnel_old = tunnel_set->tunnels[i];
        if (strcmp(tunnel_old->url, tunnel->url) == 0)
        {
            tunnel_set->tunnels[i] = tunnel;
            return tunnel_old;
        }
    }

    return NULL;
}

/*
 * Remove a tunnel from a tunnel_set_s.
 */
static tunnel_t tunnel_set_delete(tunnel_set_t tunnel_set, const char *url)
{
    for (size_t i = 0; i < tunnel_set->length; i++)
    {
        if (strcmp(tunnel_set->tunnels[i]->url, url) == 0)
        {
            tunnel_t tunnel = tunnel_set->tunnels[i];
            for (; i < tunnel_set->length-1; i++)
            {
                tunnel_set->tunnels[i] = tunnel_set->tunnels[i+1];
            }
            tunnel_set->length--;
            return tunnel;
        }
    }
    return NULL;
}

/*
 * Find a tunnel in a tunnel_set_s.
 */
static int tunnel_set_lookup(tunnel_set_t tunnel_set, const char *url)
{
    for (size_t i = 0; i < tunnel_set->length; i++)
    {
        if (strcmp(tunnel_set->tunnels[i]->url, url) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Initialise this module.
 */
void tunnel_init(void)
{
    thread_lock_init(&tunnels_lock);
    rng = random_init();
    http_register_callback("tunnels-active.html", tunnel_active_html);
    http_register_callback("tunnels-all.html", tunnel_all_html);
}

/*
 * Tunnel file load.
 */
void tunnel_file_read(void)
{
    const char *filename = TUNNELS_FILENAME;
    FILE *file = fopen(filename, "r");
    if (file == NULL)
    {
        warning("unable to open tunnel cache file \"%s\" for reading; "
            "will use backup tunnel cache file \"%s\"", TUNNELS_FILENAME,
            TUNNELS_BAK_FILENAME);
        filename = TUNNELS_BAK_FILENAME;
        file = fopen(filename, "r");
        if (file == NULL)
        {
            warning("unable to open backup tunnel cache file \"%s\" for "
                "reading", filename);
            return;
        }
    }

    while (true)
    {
        char url[CKTP_MAX_URL_LENGTH+1];
        size_t i = 0;
        url[i] = getc(file);
        if (url[i] == EOF)
        {
            if (ferror(file))
            {
                warning("unable to read tunnel URL from file \"%s\"",
                    filename);
                break;
            }
            if (feof(file))
            {
                break;
            }
        }
        if (url[i] == '\n')
        {
            continue;
        }
        if (url[i] == '#')
        {
            char c;
            while ((c = getc(file)) != '\n' && c != EOF)
                ;
            continue;
        }
        while (url[i] != ' ' && i < CKTP_MAX_URL_LENGTH && !feof(file) &&
                !ferror(file))
        {
            i++;
            url[i] = getc(file);
        }
        if (i == 0 || url[i] != ' ')
        {
            warning("unable to read tunnel URL from file \"%s\"; expected "
                "1 or more URL characters followed by a space character",
                filename);
            break;
        }
        url[i] = '\0';

        unsigned age;
        if (fscanf(file, "%u", &age) != 1 || age > UINT8_MAX)
        {
            warning("unable to read age for tunnel %s from file \"%s\"",
                url, filename);
            break;
        }
        if (getc(file) != '\n')
        {
            warning("unable to read tunnel %s file from file \"%s\"; "
                "missing end-of-line character", url, filename);
            break;
        }

        tunnel_t tunnel = tunnel_create(url, (uint8_t)age);
        thread_lock(&tunnels_lock);
        tunnel_set_insert(&tunnels_cache, tunnel);
        thread_unlock(&tunnels_lock);
    }

    fclose(file);
}

/*
 * Tunnels file write.
 */
void tunnel_file_write(void)
{
    thread_lock(&tunnels_lock);
#ifdef WINDOWS
    remove(TUNNELS_BAK_FILENAME);
#endif
    if (rename(TUNNELS_FILENAME, TUNNELS_BAK_FILENAME) != 0)
    {
        warning("unable to backup old tunnel cache file \"%s\" to \"%s\"",
            TUNNELS_FILENAME, TUNNELS_BAK_FILENAME);
    }
    FILE *file = fopen(TUNNELS_TMP_FILENAME, "w");
    if (file == NULL)
    {
        warning("unable to open tunnel cache file \"%s\" for writing",
            TUNNELS_FILENAME);
        return;
    }

    fprintf(file, "# %s tunnel cache\n", PROGRAM_NAME_LONG);
    fputs("# AUTOMATICALLY GENERATED, DO NOT EDIT\n\n", file);
    for (size_t i = 0; i < tunnels_cache.length; i++)
    {
        tunnel_t tunnel = tunnels_cache.tunnels[i];
        if (tunnel->age != 0)
        {
            fprintf(file, "# AGE = %u\n", tunnel->age);
            fprintf(file, "%s %u\n\n", tunnel->url, tunnel->age);
        }
    }
    fclose(file);

#ifdef WINDOWS
    remove(TUNNELS_FILENAME);   // For windows rename() bug.
#endif
    if (rename(TUNNELS_TMP_FILENAME, TUNNELS_FILENAME) != 0)
    {
        warning("unable to move temporary tunnel cache file \"%s\" to \"%s\"",
            TUNNELS_TMP_FILENAME, TUNNELS_FILENAME);
    }
    thread_unlock(&tunnels_lock);
}

/*
 * Initialise a tunnel.
 */
static tunnel_t tunnel_create(const char *url, uint8_t age)
{
    tunnel_t tunnel = (tunnel_t)malloc(sizeof(struct tunnel_s));
    if (tunnel == NULL)
    {
        error("unable to allocate " SIZE_T_FMT " bytes for tunnel data "
            "structure", sizeof(struct tunnel_s));
    }

    static uint16_t id = 0;
    tunnel->tunnel    = NULL;
    tunnel->age       = age;
    tunnel->state     = TUNNEL_STATE_CLOSED;
    tunnel->reconnect = false;
    tunnel->id        = id++;
    tunnel->weight    = 1.0;
    strncpy(tunnel->url, url, CKTP_MAX_URL_LENGTH);
    tunnel->url[CKTP_MAX_URL_LENGTH] = '\0';
    return tunnel;
}

/*
 * Close a tunnel.
 */
static void tunnel_free(tunnel_t tunnel)
{
    if (tunnel != NULL)
    {
        switch (tunnel->state)
        {
            case TUNNEL_STATE_OPENING:
                tunnel->state = TUNNEL_STATE_DELETING;
                break;
            case TUNNEL_STATE_DELETING:
                break;
            default:
                cktp_close_tunnel(tunnel->tunnel);
                free(tunnel);
        }
    }
}

/*
 * Open (activate) some tunnels for use.
 */
void tunnel_open(void)
{
    thread_t thread1;
    thread_create(&thread1, tunnel_activate_manager, NULL);
    thread_t thread2;
    thread_create(&thread2, tunnel_reconnect_manager, NULL);
}

/*
 * Check if there is currently an active tunnel available or not.
 */
bool tunnel_ready(void)
{
    thread_lock(&tunnels_lock);
    bool result = (tunnels_active.length != 0);
    thread_unlock(&tunnels_lock);
    return result;
}

/*
 * Tunnel activator thread.
 */
#define MAX_INIT_OPEN               8
static void *tunnel_activate_manager(void *unused)
{
    do
    {
        // Attempt to open new tunnels.
        thread_lock(&tunnels_lock);
        size_t max = MAX_INIT_OPEN - tunnels_active.length + 1;
        size_t j = 0;
        for (size_t i = 0; j < max && i < tunnels_cache.length; i++)
        {
            tunnel_t tunnel = tunnels_cache.tunnels[i];
            if (tunnel->state == TUNNEL_STATE_CLOSED)
            {
                j++;
                tunnel->state = TUNNEL_STATE_OPENING;
                thread_t thread;
                thread_create(&thread, tunnel_activate, (void *)tunnel);
            }
        }
        uint64_t stagger = random_uint64(rng);
        thread_unlock(&tunnels_lock);

        if (j == max)
        {
            break;
        }

        // Wait for some tunnels to open:
        sleeptime(150*SECONDS + (stagger % 10000) * MILLISECONDS);
    }
    while (tunnels_active.length < MAX_INIT_OPEN);

    return NULL;
}

/*
 * Queue a tunnel for use.
 */
#define MAX_RETRIES                 3
static void *tunnel_activate(void *tunnel_ptr)
{
    tunnel_t tunnel = (tunnel_t)tunnel_ptr;
    bool result = tunnel_try_activate(tunnel);
    thread_lock(&tunnels_lock);
    switch (tunnel->state)
    {
        case TUNNEL_STATE_DELETING:
            tunnel->state = TUNNEL_STATE_OPEN;
            tunnel_free(tunnel);
            break;
        case TUNNEL_STATE_CLOSING:
            tunnel->state = TUNNEL_STATE_CLOSED;
            cktp_close_tunnel(tunnel->tunnel);
            tunnel->tunnel = NULL;
            break;
        case TUNNEL_STATE_OPENING:
            if (result)
            {
                log("successfully opened tunnel %s", tunnel->url);
                tunnel->state = TUNNEL_STATE_OPEN;
                tunnel->age   = TUNNEL_INIT_AGE;
                tunnel_set_insert(&tunnels_active, tunnel);
            }
            else
            {
                log("unable to open tunnel %s; giving up", tunnel->url);
                tunnel->state = TUNNEL_STATE_DEAD;
                tunnel->age = (tunnel->age == 0? 0: tunnel->age - 1);
            }
            break;
        default:
            panic("unexpected tunnel state %u", tunnel->state);
    }
    thread_unlock(&tunnels_lock);

    tunnel_file_write();
    return NULL;
}

/*
 * Attempt to activate the given tunnel.
 */
static bool tunnel_try_activate(tunnel_t tunnel)
{
    thread_lock(&tunnels_lock);
    uint64_t stagger = (random_uint32(rng) % 1000) * MILLISECONDS;
    thread_unlock(&tunnels_lock);
    uint64_t retry_time_us = 10*SECONDS + stagger;
    unsigned retries = MAX_RETRIES;
    while (tunnel->state == TUNNEL_STATE_OPENING)
    {
        log("attempting to open tunnel %s", tunnel->url);
        tunnel->tunnel = cktp_open_tunnel(tunnel->url);
        if (tunnel->tunnel != NULL)
        {
            return true;
        }
        if (tunnel->state != TUNNEL_STATE_OPENING)
        {
            break;
        }
        retries--;
        if (retries == 0)
        {
            return false;
        }
        log("unable to open tunnel %s; retrying in %.1f seconds", tunnel->url,
            (double)retry_time_us / (double)SECONDS);
        sleeptime(retry_time_us);
        retry_time_us *= 6;
    }
    return true;
}

/*
 * Tunnel a packet.
 */
bool tunnel_packets(uint8_t *packet, uint8_t **packets, uint64_t hash,
    unsigned repeat, uint16_t config_mtu)
{
    thread_lock(&tunnels_lock);

    // Select a tunnel for this packet:
    tunnel_t tunnel = tunnel_get(hash, repeat);
    if (tunnel == NULL)
    {
        thread_unlock(&tunnels_lock);
        warning("unable to tunnel packet (no suitable tunnel is open); "
            "the packet will be dropped");
        return false;
    }

    // Check if any tunneled packet is too big:
    uint16_t mtu = cktp_tunnel_get_mtu(tunnel->tunnel, config_mtu);
    if (mtu == 0)
    {
        return false;
    }
    bool fit = true;
    for (size_t i = 0; packets[i] != NULL; i++)
    {
        size_t tot_len = ntohs(((struct iphdr *)packets[i])->tot_len);
        fit = fit && (tot_len <= mtu);
        if (!fit)
        {
            log("unable tunnel packet of size " SIZE_T_FMT " bytes; maximum "
                "allowed packet size is %u bytes", tot_len, mtu);
        }
    }
    if (!fit)
    {
        cktp_fragmentation_required(tunnel->tunnel, mtu, packet);
        thread_unlock(&tunnels_lock);
        return true;
    }
    
    // Tunnel the packets:
    for (size_t i = 0; packets[i] != NULL; i++)
    {
        cktp_tunnel_packet(tunnel->tunnel, packets[i]);
    }

    thread_unlock(&tunnels_lock);
    return true;
}

/*
 * Given a hash, return a tunnel to use.  Return NULL if none are avaiable.
 */
static tunnel_t tunnel_get(uint64_t hash, unsigned repeat)
{
    static struct tunnel_history_s tunnel_history[TUNNEL_HISTORY_SIZE];

    if (tunnels_active.length == 0)
    {
        return NULL;
    }
    size_t hist_idx = (size_t)(hash % TUNNEL_HISTORY_SIZE);
    uint32_t hist_hash = (uint32_t)(hash ^ (hash >> 32));
    uint32_t weight_hash = hist_hash * (repeat + 1);
    double total_weight = 0.0;
    for (size_t i = 0; i < tunnels_active.length; i++)
    {
        total_weight += tunnels_active.tunnels[i]->weight;
    }
    double pick = ((double)weight_hash / (double)UINT32_MAX) * total_weight;

    size_t idx;
    for (idx = 0; idx < tunnels_active.length &&
            pick >= tunnels_active.tunnels[idx]->weight; idx++)
    {
        pick -= tunnels_active.tunnels[idx]->weight;
    }
    tunnel_t tunnel = tunnels_active.tunnels[idx];

    if (repeat != 0)
    {
        // This packet has been repeated.  This can be for many reasons,
        // but here we factor in the possibility that the tunnel is down, or
        // congested.  We adjust weights to make it less likely the last
        // selected tunnel will be chosen again in the future.
        tunnel_t bad_tunnel = NULL;
        if (tunnel_history[hist_idx].hash == hist_hash)
        {
            // Punish the tunnel that failed to send the packet:
            for (size_t i = 0; i < tunnels_active.length; i++)
            {
                if (tunnels_active.tunnels[i]->id ==
                        tunnel_history[hist_idx].id)
                {
                    bad_tunnel = tunnels_active.tunnels[i];
                    pick -= bad_tunnel->weight;
                    bad_tunnel->weight = bad_tunnel->weight * 0.75;
                    bad_tunnel->weight = (bad_tunnel->weight < 0.005?
                        0.005: bad_tunnel->weight);
                    break;
                }
            }
        }

        // Pick a different tunnel (if possible)
        if (tunnel == bad_tunnel)
        {
            idx = (idx + 1) % tunnels_active.length;
            tunnel = tunnels_active.tunnels[idx];
        }
    }

    // Assume success -- adjust weight accordingly
    tunnel->weight = tunnel->weight + 0.15 * tunnel->weight;
    tunnel->weight = (tunnel->weight > 1.0? 1.0: tunnel->weight);

    // Record this packet into the tunnel history:
    tunnel_history[hist_idx].hash = hist_hash;
    tunnel_history[hist_idx].id   = tunnel->id;

    return tunnel;
}

/*
 * Add a tunnel.
 */
void tunnel_add(const char *url)
{
    // First check if the URL is syntactically valid:
    if (!cktp_parse_url(url, NULL, NULL, NULL, NULL))
    {
        return;
    }

    thread_lock(&tunnels_lock);
    int idx = tunnel_set_lookup(&tunnels_cache, url);
    tunnel_t tunnel;
    if (idx < 0)
    {
        tunnel = tunnel_create(url, TUNNEL_INIT_AGE);
        tunnel_set_insert(&tunnels_cache, tunnel);
    }
    else
    {
        tunnel = tunnels_cache.tunnels[idx];
        if (tunnel->state == TUNNEL_STATE_OPEN ||
            tunnel->state == TUNNEL_STATE_OPENING)
        {
            thread_unlock(&tunnels_lock);
            warning("unable to add tunnel %s; tunnel is already open or "
                "opening", url);
            return;
        }
    }
    tunnel->state = TUNNEL_STATE_OPENING;
    thread_unlock(&tunnels_lock);
    log("added tunnel %s", url);
    thread_t thread;
    thread_create(&thread, tunnel_activate, (void *)tunnel);

    tunnel_file_write();
}

/*
 * Delete a tunnel.
 */
void tunnel_delete(const char *url)
{
    thread_lock(&tunnels_lock);
    tunnel_t tunnel = tunnel_set_delete(&tunnels_active, url);
    if (tunnel == NULL)
    {
        tunnel = tunnel_set_delete(&tunnels_cache, url);
        if (tunnel != NULL)
        {
            tunnel_free(tunnel);
            log("deleted tunnel %s", url);
        }
        else
        {
            warning("unable to delete tunnel %s; tunnel does not exist",
                url);
        }
    }
    else
    {
        // XXX: isn't (tunnel->state == TUNNEL_STATE_OPEN) an invariant for
        //      all of tunnels_active (???)
        switch (tunnel->state)
        {
            case TUNNEL_STATE_OPENING:
                tunnel->state = TUNNEL_STATE_CLOSING;
                break;
            case TUNNEL_STATE_CLOSING:
                break;
            case TUNNEL_STATE_OPEN:
                cktp_close_tunnel(tunnel->tunnel);
                tunnel->tunnel = NULL;
                tunnel->state = TUNNEL_STATE_CLOSED;
                break;
            default:
                panic("unexpected tunnel state %u", tunnel->state);
        }
        log("deactivated tunnel %s", url);
    }
    thread_unlock(&tunnels_lock);

    tunnel_file_write();
}

/*
 * Reconnection manager.
 */
static void *tunnel_reconnect_manager(void *unused)
{
    // This is the simplest possible algorithm: continiously poll each tunnel
    // to see if it needs to reconnect.  We could implement something more
    // sophisticated here -- however that's a lot of implementation effort for
    // almost no benefit.  This loop will consume very little CPU time.
    while (true)
    {
        thread_lock(&tunnels_lock);
        uint64_t stagger = (random_uint32(rng) % 1000) * MILLISECONDS;
        thread_unlock(&tunnels_lock);
        sleeptime(1*SECONDS + stagger);
        thread_lock(&tunnels_lock);
        uint64_t currtime = gettime();
        for (size_t i = 0; i < tunnels_active.length; i++)
        {
            tunnel_t tunnel = tunnels_active.tunnels[i];
            
            // tunnel->reconnect ensures we only try to reconnect once.
            if (!tunnel->reconnect &&
                cktp_tunnel_timeout(tunnel->tunnel, currtime))
            {
                tunnel->reconnect = true;
                char *url = strdup(tunnel->url);
                thread_t thread;
                thread_create(&thread, tunnel_reconnect, (void *)url); 
            }
        }
        thread_unlock(&tunnels_lock);
    }

    return NULL;
}

/*
 * Reconnect the tunnel.
 */
static void *tunnel_reconnect(void *url_ptr)
{
    // Attempt to reconnect to the given URL.  This basically works by
    // creating a completely new tunnel instance, and then replacing the
    // old instance.

    char *url = (char *)url_ptr;
    tunnel_t tunnel = tunnel_create(url, TUNNEL_INIT_AGE);
    free(url);

    tunnel->state = TUNNEL_STATE_OPENING;
    bool result = tunnel_try_activate(tunnel);
    if (result)
    {
        // Success, replace the old tunnel with the new version:
        thread_lock(&tunnels_lock);
        tunnel_t replaced_active = tunnel_set_replace(&tunnels_active, tunnel);
        tunnel_t replaced_cache  = tunnel_set_replace(&tunnels_cache, tunnel);
        bool found = false;
        if (replaced_active != NULL)
        {
            found = true;
            tunnel_free(replaced_active);
        }
        else if (replaced_cache != NULL)
        {
            found = true;
            cktp_close_tunnel(tunnel->tunnel);
            tunnel->tunnel    = NULL;
            tunnel->state     = TUNNEL_STATE_DEAD;
            tunnel->reconnect = false;
            tunnel_free(replaced_cache);
        }
        thread_unlock(&tunnels_lock);
        if (!found)
        {
            // Tunnel has been deactivated, no need for reopened tunnel
            tunnel_free(tunnel);
        }
        else
        {
            log("successfully (re)opened tunnel %s", tunnel->url);
        }
    }
    else
    {
        warning("unable to (re)open tunnel %s; tunnel will be deactivated",
            tunnel->url);

        // Failure, we could not (re)open the tunnel.  We assume the tunnel
        // is now dead, so deactivate it here.
        thread_lock(&tunnels_lock);
        tunnel_t old_tunnel = tunnel_set_delete(&tunnels_active, url);
        if (old_tunnel != NULL)
        {
            cktp_close_tunnel(tunnel->tunnel);
            tunnel->tunnel    = NULL;
            tunnel->state     = TUNNEL_STATE_DEAD;
            tunnel->reconnect = false;
        }
        thread_unlock(&tunnels_lock);
        tunnel_free(tunnel);
    }
    return NULL;
}

