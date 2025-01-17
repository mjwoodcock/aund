/*-
 * Copyright (c) 2010 Simon Tatham
 * Copyright (c) 1998, 2010 Ben Harris
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * fs_cli.c - command-line interpreter for file server
 */

#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <grp.h>
#include <libgen.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utmp.h>

#include "aun.h"
#include "fs_proto.h"
#include "fs_errors.h"
#include "extern.h"
#include "fileserver.h"

typedef void fs_cmd_impl(struct fs_context *, char *);

struct fs_cmd {
    char    *name;
    int     minlen;   /* Min length of command for abreviation */
    fs_cmd_impl *impl;
};

static fs_cmd_impl fs_cmd_bye;
static fs_cmd_impl fs_cmd_cat;
static fs_cmd_impl fs_cmd_cdir;
static fs_cmd_impl fs_cmd_delete;
static fs_cmd_impl fs_cmd_dir;
static fs_cmd_impl fs_cmd_fsopt;
static fs_cmd_impl fs_cmd_i_am;
static fs_cmd_impl fs_cmd_info;
static fs_cmd_impl fs_cmd_lib;
static fs_cmd_impl fs_cmd_load;
static fs_cmd_impl fs_cmd_save;
static fs_cmd_impl fs_cmd_sdisc;
static fs_cmd_impl fs_cmd_pass;
static fs_cmd_impl fs_cmd_priv;
static fs_cmd_impl fs_cmd_rename;
static fs_cmd_impl fs_cmd_access;
static fs_cmd_impl fs_cmd_newuser;
static fs_cmd_impl fs_cmd_deluser;

static bool fs_cli_match(char *cmdline, char **tail, const struct fs_cmd *cmd);
static void fs_cli_unrec(struct fs_context *, char *);

/* the table has command, number of character to make it */
/* unique (abbreviation) and the command to call when */
/* found. */

static const struct fs_cmd cmd_tab[] = {
    {"BYE",     1, fs_cmd_bye,      },
    {"CAT",     0, fs_cmd_cat,      },
    {"CDIR",    2, fs_cmd_cdir,     },
    {"DELETE",  3, fs_cmd_delete,   },
    {"DIR",     3, fs_cmd_dir,      },
    {"FSOPT",   2, fs_cmd_fsopt,    },
    {"INFO",    1, fs_cmd_info,     },
    {"I AM",    2, fs_cmd_i_am,     },
    {"LIB",     3, fs_cmd_lib,      },
    {"LOAD",    1, fs_cmd_load,     },
    {"LOGOFF",  3, fs_cmd_bye,      },
    {"PASS",    1, fs_cmd_pass,     },
    {"PRIV",    2, fs_cmd_priv,     },
    {"RENAME",  1, fs_cmd_rename,   },
    {"SAVE",    1, fs_cmd_save,     },
    {"SDISC",   3, fs_cmd_sdisc,    },
    {"ACCESS",  2, fs_cmd_access,   },
    {"NEWUSER", 2, fs_cmd_newuser,    }, 
    {"REMUSER", 3, fs_cmd_deluser,    },
};

#define NCMDS (sizeof(cmd_tab) / sizeof(cmd_tab[0]))

/*
 * Handle a command-line packet from a client.
 */
void
fs_cli(struct fs_context *c)
{
    int i;
    char *head, *tail, *backup;

    c->req->data[strcspn(c->req->data, "\r")] = '\0';

    if (debug) printf("cli ");
    head = backup = strdup(c->req->data);
    while (strchr("* \t", *head)) head++;
    if (!*head) {
        struct ec_fs_reply reply;

        if (debug) printf("[%s] -> ignore\n", c->req->data);
        reply.command_code = EC_FS_CC_DONE;
        reply.return_code = EC_FS_RC_OK;
        fs_reply(c, &reply, sizeof(reply));
        free(backup);
        return;
    }
    for (i = 0; i < NCMDS; i++) {
        if (fs_cli_match(head, &tail, &(cmd_tab[i]))) {
            /*
             * We print a diagnostic of the command for
             * all commands except *I AM and *PASS.
             */
            if (debug) {
                if (cmd_tab[i].impl == fs_cmd_i_am ||
                    cmd_tab[i].impl == fs_cmd_pass)
                    printf("[%.*s <hidden>]",
    // loop and accept new connections
                        (int)(tail - backup), backup);
                else
                    printf("[%s]", c->req->data);
            }
            (cmd_tab[i].impl)(c, tail);
            break;
        }
    }
    if (i == NCMDS) {
        if (debug)
            printf("[%s]", c->req->data);
        fs_cli_unrec(c, backup);
    }
    free(backup);
}

static void
fs_cli_unrec(struct fs_context *c, char *cmd)
{
    struct ec_fs_reply *reply;

    if (debug) printf("[%s] -> <unrecognised>\n", cmd);
    reply = malloc(sizeof(*reply) + strlen(cmd) + 1);
    reply->command_code = EC_FS_CC_UNREC;
    reply->return_code = EC_FS_RC_OK;
    strcpy(reply->data, cmd);
    reply->data[strlen(cmd)] = '\r';
    fs_reply(c, reply, sizeof(*reply) + strlen(cmd) + 1);
    free(reply);
}

/*
 * Work out if cmdline starts with an acceptable abbreviation for
 * cmd. If it does, return the tail of the command beyond that.
 */

static bool
fs_cli_match(char *cmdline, char **tail, const struct fs_cmd *cmd)
{
    int i;

    for (i = 0;; i++) {
        int creal = cmd->name[i];
        int cthis = toupper((unsigned char)cmdline[i]);

        if (creal == '\0') {   /* real command has just ended */
            if (strchr(" .^&@$%", cthis)) {   /* so has input */
                *tail = cmdline + i;
                return true;
            } else {
                return false;   /* no match */
            }
        }
        if (cthis == '.') {    /* input command is abbreviated */
            if (i < cmd->minlen)
                return false;   /* too short an abbreviation */
            *tail = cmdline + i + 1;   /* skip the dot */
            return true;      /* abbreviation which matches */
        }
        if (creal != cthis)
            return false;      /* mismatched character */
    }
}

/*
 * A bit like strsep, only different.  Breaks off the first word
 * (vaguely defined) of the input.  Afterwards, returns a pointer to
 * the first word (null-terminated) and points stringp at the start of
 * the tail.  Destroys the input in the process.
 */

char *
fs_cli_getarg(char **stringp)
{
    char *start;
    /* Skip leading whitespace */
    for (; **stringp == ' '; (*stringp)++);
    switch (**stringp) {
    case '"':
        /* Quoted string. */
        /*
         * XXX There seems to be no way to embed double quotes
         * in a quoted string (or at least, NetFiler doesn't
         * know how).  For now, assume the first '"' ends the
         * string.
         */
        (*stringp)++;
        start = *stringp;
        *stringp = strchr(start, '"');
        if (*stringp == NULL)
            /* Badness -- unterminated quoted string. */
            *stringp = strchr(start, '\0');
        else {
            **stringp = '\0';
            (*stringp)++;
        }
        break;
    case '\0':
        /* End of string hit.  Return two null strings. */
        start = *stringp;
        break;
    default:
        /* Unquoted.  Terminates at next space or end. */
        start = *stringp;
        *stringp = strchr(start, ' ');
        if (*stringp == NULL)
            *stringp = strchr(start, '\0');
        else {
            **stringp = '\0';
            (*stringp)++;
        }
    }
    return start;
}

static void
fs_cmd_i_am(struct fs_context *c, char *tail)
{
    struct ec_fs_reply_logon reply;
    char *login, *password, *oururd;
    int opt4;

    login = fs_cli_getarg(&tail);
    if (isdigit((unsigned char)login[0]))
        /* Client passed us a station number.  Skip it. */
        login = fs_cli_getarg(&tail);
    password = fs_cli_getarg(&tail);
    if (debug) printf(" -> log on [%s]\n", login);
    oururd = userfuncs->validate(login, password, &opt4);
    if (!oururd) {
        fs_err(c, EC_FS_E_WRONGPW);
        return;
    }
    /*
     * They're authenticated, so add them to the list of clients.
     * First, we see if this client's already logged on, and if
     * so, log them off first.
     */
    if (c->client)
        fs_delete_client(c->client);
    c->client = fs_new_client(c->from);
    if (c->client == NULL) {
        fs_error(c, 0xff, "Internal server error");
        return;
    }
    c->client->login = strdup(login);
    c->client->priv = userfuncs->get_priv(c->client->login);
        if (debug) printf("Cli: %s has %d\n", c->client->login, c->client->priv);
    reply.std_tx.command_code = EC_FS_CC_LOGON;
    reply.std_tx.return_code = EC_FS_RC_OK;
    /*
     * Initial user environment.  Note that we can't use the same
     * handle twice.
     *
    */
    if (debug) printf("Env: URD: %s CSD: %s LIB: %s\n", oururd, oururd, lib);
                
    reply.urd = fs_open_handle(c->client, oururd, O_RDONLY, false);
    reply.csd = fs_open_handle(c->client, oururd, O_RDONLY, false);
    reply.lib = fs_open_handle(c->client, lib, O_RDONLY, false);
    reply.opt4 = opt4;
    if (debug) printf("returning: urd=%d, csd=%d, lib=%d, opt4=%d\n",
              reply.urd, reply.csd, reply.lib, reply.opt4);
    fs_reply(c, &(reply.std_tx), sizeof(reply));
}

static void
fs_cmd_priv(struct fs_context *c, char *tail)
{
    struct ec_fs_reply reply;
    char *user, *priv;

    user = fs_cli_getarg(&tail);
    priv = fs_cli_getarg(&tail);
    if (debug) printf("cli: priv request %s to '%s'\n", user, priv);
    if (c->client == NULL) {
        fs_err(c, EC_FS_E_WHOAREYOU);
        return;
    }
    // Add Validation to the privilege level
    if (strlen(priv) > 1) 
    {
        // TODO: Write a procedure to check that the priv flag is valid
        fs_err(c, EC_FS_E_NOPRIV);
        return;
    }

    if (userfuncs->set_priv(c->client, user, priv)) {
        fs_err(c, EC_FS_E_NOPRIV);  // Should be Priv??
        return;
    }
    reply.command_code = EC_FS_CC_DONE;
    reply.return_code = EC_FS_RC_OK;
    fs_reply(c, &reply, sizeof(reply));
}

static void
fs_cmd_pass(struct fs_context *c, char *tail)
{
    struct ec_fs_reply reply;
    char *oldpw, *newpw;
    oldpw = fs_cli_getarg(&tail);
    newpw = fs_cli_getarg(&tail);
    if (debug) printf("cli: change password\n");
    if (c->client == NULL) {
        fs_err(c, EC_FS_E_WHOAREYOU);
        return;
    }
    if (userfuncs->change(c->client->login, oldpw, newpw)) {
        fs_err(c, EC_FS_E_BADPW);
        return;
    }
    reply.command_code = EC_FS_CC_DONE;
    reply.return_code = EC_FS_RC_OK;
    fs_reply(c, &reply, sizeof(reply));
}

/*
 * This bit of the protocol doesn't seem to be documented anywhere,
 * but the System 3 NOS (at least V.IIIP) doesn't interpret *CAT
 * itself.  The details here are reverse-engineered from the behaviour
 * of an old Acorn file server.
 */
static void
fs_cmd_cat(struct fs_context *c, char *tail)
{
    struct ec_fs_reply *reply;
    char *path;

    path = fs_cli_getarg(&tail);
    if (debug) printf(" -> cat [%s]\n", path);
    reply = malloc(sizeof(*reply) + strlen(path) + 1);
    reply->command_code = EC_FS_CC_CAT;
    reply->return_code = EC_FS_RC_OK;
    strcpy(reply->data, path);
    reply->data[strlen(path)] = '\r';
    fs_reply(c, reply, sizeof(*reply) + strlen(path) + 1);
    free(reply);
}

static void
fs_cmd_rename(struct fs_context *c, char *tail)
{
    struct ec_fs_reply reply;
    struct ec_fs_meta meta;
    char *oldname, *newname;
    char *oldupath, *newupath;
    char *path_argv[2];
    FTS *ftsp;
    FTSENT *f;
    bool is_owner = false;

    oldname = fs_cli_getarg(&tail);
    newname = fs_cli_getarg(&tail);
    if (debug) printf(" -> rename [%s,%s]\n", oldname, newname);
    if (c->client == NULL) {
        fs_err(c, EC_FS_E_WHOAREYOU);
        return;
    }
    if ((oldupath = fs_unixify_path(c, oldname)) == NULL) return;
    if ((newupath = fs_unixify_path(c, newname)) == NULL) {
        free(oldupath);
        return;
    }
        path_argv[0] = oldupath;
        path_argv[1] = NULL;
        ftsp = fts_open(path_argv, FTS_LOGICAL, NULL);
        f = fts_read(ftsp);

        if (f->fts_statp->st_mode & S_IXUSR)
        {
            // file is locked so we cannot rename
            fs_err(c, EC_FS_E_LOCKED);
            goto locked;
        }

        is_owner = fs_is_owner(c, oldupath);

        if (is_owner == true)
        {
            if (!(f->fts_statp->st_mode & S_IWUSR))
            {
            // We dont have write access
            fs_err(c, EC_FS_E_NOACCESS);
            fts_close(ftsp);
            goto notallowed;
            }
        }
        if (is_owner == false)
        {
            if (!(f->fts_statp->st_mode & S_IWOTH))
            {
            // We dont have write access
            fs_err(c, EC_FS_E_NOACCESS);
            fts_close(ftsp);
            goto notallowed;
            }
        }
    if (rename(oldupath, newupath) < 0) {
        free(oldupath);
        free(newupath);
        fts_close(ftsp);
        fs_errno(c);
        // Do not need to return here
    } else {
        fs_get_meta(f, &meta);
        fs_del_meta(f);
        fts_close(ftsp);

        path_argv[0] = newupath;
        path_argv[1] = NULL;
        ftsp = fts_open(path_argv, FTS_LOGICAL, NULL);
        f = fts_read(ftsp);
        fs_set_meta(f, &meta);
        fts_close(ftsp);

        reply.command_code = EC_FS_CC_DONE;
        reply.return_code = EC_FS_RC_OK;
        fs_reply(c, &reply, sizeof(reply));
    }
notallowed:
    free(oldupath);
    free(newupath);
    return;

locked:
    fts_close(ftsp);
    free(oldupath);
    free(newupath);
    return;    
}

static void
fs_cmd_cdir(struct fs_context *c, char *tail)
{
    char *path;

    path = fs_cli_getarg(&tail);
    if (debug) printf(" -> cdir [%s]\n", path);
    if (*path)
        fs_cdir1(c, path);
    else
        fs_error(c, 0xff, "Syntax");
}

static void
fs_cmd_delete(struct fs_context *c, char *tail)
{
    char *path;

    path = fs_cli_getarg(&tail);
    if (debug) printf(" -> delete [%s]\n", path);
    if (*path)
        fs_delete1(c, path);
    else
        fs_error(c, 0xff, "Syntax");
}

static void
fs_cmd_sdisc(struct fs_context *c, char *tail)
{
    struct ec_fs_reply_sdisc reply;
    char *oururd;

    if (debug) printf(" -> sdisc\n");
    if (c->client == NULL) {
        fs_err(c, EC_FS_E_WHOAREYOU);
        return;
    }
    oururd = userfuncs->urd(c->client->login);
    if (!oururd) {
        fs_error(c, 0xff, "Failed lookup");
        return;
    }
    reply.std_tx.command_code = EC_FS_CC_SDISC;
    reply.std_tx.return_code = EC_FS_RC_OK;
    /*
     * Reset user environment.  Note that we can't use the same
     * handle twice.
     */
    fs_close_handle(c->client, c->req->urd);
    fs_close_handle(c->client, c->req->csd);
    fs_close_handle(c->client, c->req->lib);
    reply.urd = fs_open_handle(c->client, oururd, O_RDONLY, false);
    reply.csd = fs_open_handle(c->client, oururd, O_RDONLY, false);
    reply.lib = fs_open_handle(c->client, lib, O_RDONLY, false);
    fs_reply(c, &(reply.std_tx), sizeof(reply));
}

static void
fs_cmd_dir(struct fs_context *c, char *tail)
{
    char *upath;
    struct ec_fs_reply_dir reply;
    int priv = EC_FS_PRIV_NONE;

    if (c->client == NULL) {
        fs_err(c, EC_FS_E_WHOAREYOU);
        return;
    }

    // Check if the user has fixed privilege, if they do
    // they are not allowed to change directory
    priv = userfuncs->get_priv(c->client->login);
    if (priv == EC_FS_PRIV_FIXED)
    {
        fs_error(c, 0xff, "Not allowed");
        return;
    }
    upath = fs_cli_getarg(&tail);
    if (!*upath)
        upath = "&";
    if (debug) printf(" -> dir [%s]\n", upath);
    if ((upath = fs_unixify_path(c, upath)) == NULL) return;
    reply.new_handle = fs_open_handle(c->client, upath, O_RDONLY, false);
    free(upath);
    if (reply.new_handle == 0) {
        fs_errno(c);
        return;
    }
    if (c->client->handles[reply.new_handle]->type != FS_HANDLE_DIR) {
        fs_close_handle(c->client, reply.new_handle);
        fs_err(c, EC_FS_E_NOTDIR);
        return;
    }
    fs_close_handle(c->client, c->req->csd);
    reply.std_tx.command_code = EC_FS_CC_DIR;
    reply.std_tx.return_code = EC_FS_RC_OK;
    fs_reply(c, &(reply.std_tx), sizeof(reply));
}

static void
fs_cmd_lib(struct fs_context *c, char *tail)
{
    char *upath;
    struct ec_fs_reply_dir reply;

    if (c->client == NULL) {
        fs_err(c, EC_FS_E_WHOAREYOU);
        return;
    }
    upath = fs_cli_getarg(&tail);
    if (!*upath) {
        if (debug) printf(" -> default lib\n");
        reply.new_handle =
            fs_open_handle(c->client, lib, O_RDONLY, false);
    } else {
        if (debug) printf(" -> lib [%s]\n", upath);
        if ((upath = fs_unixify_path(c, upath)) == NULL) return;
        reply.new_handle =
            fs_open_handle(c->client, upath, O_RDONLY, false);
        free(upath);
    }
    if (reply.new_handle == 0) {
        fs_errno(c);
        return;
    }
    if (c->client->handles[reply.new_handle]->type != FS_HANDLE_DIR) {
        fs_close_handle(c->client, reply.new_handle);
        fs_err(c, EC_FS_E_NOTDIR);
        return;
    }
    fs_close_handle(c->client, c->req->lib);
    reply.std_tx.command_code = EC_FS_CC_LIB;
    reply.std_tx.return_code = EC_FS_RC_OK;
    fs_reply(c, &(reply.std_tx), sizeof(reply));
}


static void
fs_cmd_bye(struct fs_context *c, char *tail)
{
    /*
     * On Acorn file servers, *LOGOFF is an administrator command
     * for logging off other users' sessions, but SJ file servers
     * also supported it without arguments as an unprivileged
     * command synonymous with *BYE, so we make it an alias for
     * *BYE.
     */
    if (debug) printf(" -> logoff\n");
    fs_logoff(c);
}

void
fs_long_info(struct fs_context *c, char *string, FTSENT *f)
{
    struct ec_fs_meta meta;
    struct tm mtm, btm;
    time_t birthtime;
    unsigned long load, exec;
    char accstring[8], accstr2[8];
    mode_t currumask;
    char *acornname;
    const char *months = "janfebmaraprmayjunjulaugsepoctnovdec"; 
    int entries;

    acornname = strdup(f->fts_name);
    fs_acornify_name(acornname);
    if (!*acornname)
        strcpy(acornname, "$");

    fs_access_to_string(accstring,
                fs_mode_to_access(f->fts_statp->st_mode));

    mtm = *localtime(&f->fts_statp->st_mtime);
    birthtime = fs_get_birthtime(f);
    btm = *localtime(&birthtime);

    if (c->client->infoformat == FS_INFO_SJ) {
        /*
         * These formats for *INFO are taken from the SJ
         * Research file server manual.
         *
         * The two three-digit hex numbers at the end of the
         * line are the primary and secondary account
         * numbers on the SJ file server that own the file.
         * I might have tried to make up something in here
         * from the Unix uid and gid, but it didn't seem
         * worth it. Though if aund actually supported
         * multiple users and enforced access control
         * between them, that would be a different matter,
         * of course.
         */
        if (S_ISDIR(f->fts_statp->st_mode)) {
            currumask = umask(777);
            umask(currumask);
            fs_access_to_string(accstr2,
                fs_mode_to_access(0777 & ~currumask));

            /*
             * Count the entries in a subdirectory.
             */
            {
                char *lastslash = strrchr(f->fts_accpath, '/');
                char *fullpath;
                char *path_argv[2];
                FTS *ftsp2;
                FTSENT *f2;

                if (lastslash)
                    lastslash++;
                else
                    lastslash = f->fts_accpath;
                fullpath = malloc((lastslash - f->fts_accpath) +
                          f->fts_namelen + 8 + 1);
                sprintf(fullpath, "%.*s/%s",
                    (int)(lastslash - f->fts_accpath),
                    f->fts_accpath, f->fts_name);
                path_argv[0] = fullpath;
                path_argv[1] = NULL;

                ftsp2 = fts_open(path_argv, FTS_LOGICAL, NULL);
                f2 = fts_read(ftsp2);
                f2 = fts_children(ftsp2, FTS_NAMEONLY);
                for (entries = 0;
                     f2 != NULL;
                     f2 = f2->fts_link) {
                    if (fs_hidden_name(f2->fts_name))
                        continue;      /* hidden file */
                    entries++;          /* count this one */
                }
                fts_close(ftsp2);
            }

            sprintf(string, "%-10.10s  Entries=%-4dDefault=%-6.6s  "
                "%-6.6s  %02d%.3s%02d %02d%.3s%02d %02d:%02d "
                "000 (000)\r\x80",
                acornname, entries, accstr2, accstring,
                btm.tm_mday,
                &months[ 3*btm.tm_mon],
                btm.tm_year % 100,
                mtm.tm_mday,
                &months[ 3*mtm.tm_mon],
                mtm.tm_year % 100,
                mtm.tm_hour,
                mtm.tm_min);
        } else {
            fs_get_meta(f, &meta);
            load =
                fs_read_val(meta.load_addr, sizeof(meta.load_addr));
            exec =
                fs_read_val(meta.exec_addr, sizeof(meta.exec_addr));
            sprintf(string, "%-10.10s %08lX %08lX     %06jX "
                "%-6.6s  %02d%.3s%02d %02d%.3s%02d %02d:%02d "
                "000 (000)\r\x80",
                acornname, load, exec,
                (uintmax_t)f->fts_statp->st_size, accstring,
                btm.tm_mday,
                &months[ 3*btm.tm_mon],
                btm.tm_year % 100,
                mtm.tm_mday,
                &months[ 3*mtm.tm_mon],
                mtm.tm_year % 100,
                mtm.tm_hour,
                mtm.tm_min);
        }
    } else {
        /*
         * This is the format specified in the RISC OS PRM,
         * except that it includes a CR at the end.  The BBC
         * and System 3/4 clients seem to expect one, though,
         * and the Level 4 Fileserver generates one, so the
         * PRM is probably wrong.
         */
        fs_get_meta(f, &meta);
        load = fs_read_val(meta.load_addr, sizeof(meta.load_addr));
        exec = fs_read_val(meta.exec_addr, sizeof(meta.exec_addr));
        sprintf(string, "%-10.10s %08lX %08lX   %06jX   "
            "%-6.6s     %02d:%02d:%02d %06x\r\x80",
            acornname, load, exec,
            (uintmax_t)f->fts_statp->st_size, accstring,
            btm.tm_mday,
            btm.tm_mon,
            btm.tm_year % 100,
            fs_get_sin(f));
    }

    free(acornname);
}

static void
fs_cmd_info(struct fs_context *c, char *tail)
{
    char *upath;
    struct ec_fs_reply *reply;
    char *path_argv[2];
    FTS *ftsp;
    FTSENT *f;

    if (c->client == NULL) {
        fs_err(c, EC_FS_E_WHOAREYOU);
        return;
    }
    upath = fs_cli_getarg(&tail);
    if (debug) printf(" -> info [%s]\n", upath);
    if ((upath = fs_unixify_path(c, upath)) == NULL) return;

    path_argv[0] = upath;
    path_argv[1] = NULL;
    ftsp = fts_open(path_argv, FTS_LOGICAL, NULL);
    f = fts_read(ftsp);
    if (f->fts_info == FTS_ERR || f->fts_info == FTS_NS) {
        fs_errno(c);
        fts_close(ftsp);
        free(upath);
        return;
    }

    reply = malloc(sizeof(*reply) + 100);
    fs_long_info(c, reply->data, f);
    reply->command_code = EC_FS_CC_INFO;
    reply->return_code = EC_FS_RC_OK;
    fs_reply(c, reply, sizeof(*reply) + strlen(reply->data));

    free(reply);
    free(upath);
    fts_close(ftsp);
}

/*
 * *SAVE and *LOAD are a bit odd in that they don't actually do
 * anything.  Instead, they just parse the command and pass back the
 * bits for the client to do the actual load or save.
 */
static void
fs_cmd_save(struct fs_context *c, char *tail)
{
    struct ec_fs_reply_cli_save *reply;
    char *path, *p;
    uint32_t start, end, exec;

    path = fs_cli_getarg(&tail);
    if (!*path) goto syntax;
    reply = malloc(sizeof(*reply) + strlen(path) + 1);
    p = fs_cli_getarg(&tail);
    if (!*p) goto syntax;
    start = strtoul(p, NULL, 16);
    p = fs_cli_getarg(&tail);
    if (!*p) goto syntax;
    end = strtoul(p, NULL, 16);
    p = fs_cli_getarg(&tail);
    if (*p)
        exec = strtoul(p, NULL, 16);
    else
        exec = start;
    if (debug) printf(" -> save [%08x, %08x, %06x, %s]\n",
        start, exec, end - start, path);
    fs_write_val(reply->meta.load_addr, start,
        sizeof(reply->meta.load_addr));
    fs_write_val(reply->meta.exec_addr, exec,
        sizeof(reply->meta.load_addr));
    fs_write_val(reply->size, end - start, sizeof(reply->size));
    strcpy(reply->path, path);
    reply->path[strlen(path)] = '\r';
    reply->std_tx.command_code = EC_FS_CC_SAVE;
    reply->std_tx.return_code = EC_FS_RC_OK;
    fs_reply(c, &reply->std_tx, sizeof(*reply) + strlen(path) + 1);
    free(reply);
    return;
syntax:
    free(reply);
    fs_error(c, 0xff, "Syntax");
}

static void
fs_cmd_load(struct fs_context *c, char *tail)
{
    struct ec_fs_reply_cli_load *reply;
    char *path, *p;
    uint32_t addr;

    path = fs_cli_getarg(&tail);
    if (!*path) goto syntax;
    reply = malloc(sizeof(*reply) + strlen(path) + 1);
    p = fs_cli_getarg(&tail);
    if (*p) {
        addr = strtoul(p, NULL, 16);
        reply->load_addr_found = 0xff;
    } else {
        addr = 0;
        reply->load_addr_found = 0;
    }
    if (debug) {
        printf(" -> load [");
        if (reply->load_addr_found)
            printf("%08x, ", addr);
        printf("%s]\n", path);
    }
    fs_write_val(reply->load_addr, addr, sizeof(reply->load_addr));
    strcpy(reply->path, path);
    reply->path[strlen(path)] = '\r';
    reply->std_tx.command_code = EC_FS_CC_LOAD;
    reply->std_tx.return_code = EC_FS_RC_OK;
    fs_reply(c, &reply->std_tx, sizeof(*reply) + strlen(path) + 1);
    free(reply);
    return;
syntax:
    free(reply);
    fs_error(c, 0xff, "Syntax");
}

static void
fs_cmd_fsopt(struct fs_context *c, char *tail)
{
    struct ec_fs_reply reply;
    char *key, *val;

    if (c->client == NULL) {
        fs_err(c, EC_FS_E_WHOAREYOU);
        return;
    }
    key = fs_cli_getarg(&tail);
    if (!*key) goto syntax;
    if (!strcasecmp(key, "infofmt")) {
        val = fs_cli_getarg(&tail);
        if (!*val) goto syntax;
        if (!strcasecmp(val, "riscos"))
            c->client->infoformat = FS_INFO_RISCOS;
        else if (!strcasecmp(val, "sj"))
            c->client->infoformat = FS_INFO_SJ;
        else
            goto syntax;
    } else  if (!strcasecmp(key, "safehandles")) {
        val = fs_cli_getarg(&tail);
        if (!*val) goto syntax;
        if (!strcasecmp(val, "true") ||
            !strcasecmp(val, "on") ||
            !strcasecmp(val, "yes"))
            c->client->safehandles = true;
        else if (!strcasecmp(val, "false") ||
            !strcasecmp(val, "off") ||
            !strcasecmp(val, "no"))
            c->client->safehandles = false;
        else
            goto syntax;
    } else

        goto syntax;
    reply.command_code = EC_FS_CC_DONE;
    reply.return_code = EC_FS_RC_OK;
    fs_reply(c, &reply, sizeof(reply));
    return;
syntax:
    fs_error(c, 0xff,"Syntax: FSOPT <OPTION> <VALUE>");
}

static void
fs_cmd_access(struct fs_context *c, char *tail)
{
    char *name, *access;
    unsigned char new_permissions;
    char *upath;
    char *path_argv[2];
    struct ec_fs_reply reply;
    uint8_t loop_count;
    bool is_owner;
    bool is_locked = false;
    bool is_owner_write = false;
    bool is_owner_read = false;
    bool is_public_write = false;
    bool is_public_read = false;
    bool found_slash = false;
    bool bad_access = false;
    FTS *ftsp;
    FTSENT *f;
    
    name = fs_cli_getarg(&tail);
    // Thought there was a bug here with the way the command line
    // Processor handled keywords with a period (.) at the end
    // but tried on the DFS and its the same, it assumes
    // *ACCESS. FILE WR/R is valid, and the filename is . and the 
    // permissions are FILE

    access = fs_cli_getarg(&tail);
    if (c->client == NULL) {
        fs_err(c, EC_FS_E_WHOAREYOU);
        return;
    }

    if (debug) printf("\nAccess File [%s] -> permissions [%s]\n", name, access);
    // FIXME:
    // Bug here *ACCESS "" WR/R is permitted
    
    if (strcmp(name, "") == 0) {
        // what file did you mean?
        fs_err(c, EC_FS_E_BADNAME);
        return;
        }

    if (strlen(name) > 10) {
        // Name is too long
        fs_err(c, EC_FS_E_BADNAME);
        return;
    }

    if (strcmp(access, "") == 0) {    
        // this is not valid
        fs_err(c, EC_FS_E_BADACCESS);
        return;
    }

    if (strlen(access) > 6) 
    {
        // Access list is too long
        fs_err(c, EC_FS_E_BADACCESS);
        return;
    }

    // The access string is not empty so now we must figure out
    // what the user gave us. 
    // added addition check for lwr/wr must not allow stuff like lww/rr etc
    
    for (loop_count = 0; (loop_count < strlen(access)); loop_count++) 
    {
        switch (access[loop_count])
        {
            case 'L':
            if (found_slash == false)
            {    
            is_locked = true;
            }
            if (found_slash == true)
            {
                bad_access = true;
                // found L after the slash
            }
            break;

            case 'W':
            if  (found_slash == false)
            {
                if (is_owner_write == true)
                {
                    bad_access = true; 
                }
                if (is_owner_write == false)
                {
                    is_owner_write = true; 
                }
            }
            if (found_slash == true)
            {
                if (is_public_write == true)
                {
                    bad_access = true;
                }
                if (is_public_write == false)
                {
                    is_public_write = true;
                }
            }
            break;

            case 'R':
            if (found_slash == false)
            {
                if (is_owner_read == true)
                {
                    bad_access = true;
                }
                if (is_owner_read == false)
                {
                    is_owner_read = true;
                }
            }
            if (found_slash == true)
            {
                if (is_public_read == true)
                {
                    bad_access = true;
                }
                if (is_public_read == false)
                {
                    is_public_read = true;
                }
            }
            break;

            case '/':
            if (found_slash == true)
            {
                bad_access = true;
            }
            if (found_slash == false)
            {
                found_slash = true;
            }
            break;

            default:
            fs_err(c, EC_FS_E_BADACCESS);
            return;
        }
    }

    // Check if the access command was malformed
    if (bad_access == true)
    {
        fs_err(c, EC_FS_E_BADACCESS);
        return;
    }

    if ((upath = fs_unixify_path(c, name )) == NULL) return;
    is_owner = fs_is_owner(c, fs_unixify_path(c, name));
    if (is_owner == false)
    {
        // We do not have owner access in this directory
        // so cannot change permissions
        fs_err(c, EC_FS_E_NOACCESS);
        return;
    }

    new_permissions = 0;
    if (is_locked)
        {
            new_permissions |= EC_FS_ACCESS_L;
        }
    if (is_owner_write)
        {
            new_permissions |= EC_FS_ACCESS_UW; 
        }
    if (is_owner_read)
        {
            new_permissions |= EC_FS_ACCESS_UR;
        }
    if (is_public_read)
        {
            new_permissions |= EC_FS_ACCESS_OR;
        }
    if (is_public_write)
        {
            new_permissions |= EC_FS_ACCESS_OW;
        }


    upath = fs_unixify_path(c, name); /* This must be freed */
    if (upath == NULL) return;
    path_argv[0] = upath;
    path_argv[1] = NULL;
    ftsp = fts_open(path_argv, FTS_LOGICAL, NULL);
    f = fts_read(ftsp);
    if (f->fts_info == FTS_ERR || f->fts_info == FTS_NS) {
        fs_errno(c);
        goto out;
    }

    if (!S_ISDIR(f->fts_statp->st_mode)) {
        /* XXX Should chose usergroup sensibly */
        if (chmod(f->fts_accpath, fs_access_to_mode(new_permissions, 0)) != 0) {
            fs_errno(c);
            goto out;
        }
    }

    if (debug) printf(" -> name [%s], permissons [%s]\n", name, access);
    reply.command_code = EC_FS_CC_DONE; 
    reply.return_code = EC_FS_RC_OK;
    fs_reply(c, &reply, sizeof(reply));
    fts_close(ftsp);
    free(upath);
    return;

out:
    fts_close(ftsp);
    free(upath);
    return;
}

static void
fs_cmd_newuser(struct fs_context *c, char *tail)
{
    struct ec_fs_reply reply;
    char *username;
    int priv = EC_FS_PRIV_NONE; /* Assume no Priv */
    bool user_exists = false;
    int result;

    username = fs_cli_getarg(&tail);

    // Check if the user is logged onto the system

    if (c->client == NULL) {
      fs_err(c, EC_FS_E_WHOAREYOU);
      return;
    }
    // check that that user has privilege
    priv = userfuncs->get_priv(c->client->login);
    if (priv != EC_FS_PRIV_SYST)
    {
        fs_err(c, EC_FS_E_NOPRIV);
        return;
    }

    // Check if new user already exists
    user_exists = userfuncs->is_user(username);

    if (user_exists == true) 
    {
        if (debug) printf("User exists was true\n");
        fs_err(c, EC_FS_E_USEREXIST);
        return;
    }

    // Now we need to add the new user

    if (username == NULL)
    {
        if (debug) printf("User was null\n");
        fs_err(c, EC_FS_E_BADUSER);
        return;
    }

    // what if the username is too long

    if (strlen(username) > 21)
    {
        if (debug) printf("Username was too long\n");
        fs_err(c, EC_FS_E_BADUSER);
        return;
    }

    // If the user does not exist add them to the password file 
    // with a blank password
    result = userfuncs->add_user(username);
    if (result == -1)
    {
        fs_error(c, 0xff, "Its all gone pear shaped\n");
        return;
    }
    // convert username to directory structure 
    // leading ./ then name changing the following . to / 

    reply.command_code = EC_FS_CC_DONE; 
    reply.return_code = EC_FS_RC_OK;
    fs_reply(c, &reply, sizeof(reply));
    return;
}

static void
fs_cmd_deluser(struct fs_context *c, char *tail)
{
    struct ec_fs_reply reply;
    char *username;
    int priv = EC_FS_PRIV_NONE; /* Assume no Priv */
    bool user_exists = false;
    int result;

    username = fs_cli_getarg(&tail);

    // Check if the user is logged onto the system

    if (c->client == NULL) {
      fs_err(c, EC_FS_E_WHOAREYOU);
      return;
    }
    // check that that user has privilege
    priv = userfuncs->get_priv(c->client->login);
    if (priv != EC_FS_PRIV_SYST)
    {
        fs_err(c, EC_FS_E_NOPRIV);
        return;
    }

    // Check if new user already exists
    if (debug) printf("Check if the user already exists\n");
    user_exists = userfuncs->is_user(username);

    if (user_exists == false)
    {
        fs_err(c, EC_FS_E_BADUSER);
        return;
    }

    if (user_exists == true)
    {
        result = userfuncs->del_user(username);
    }

    if (result == -1)
    {
        fs_error(c, 0x39, "FS Internal Error 27");
        return;
    }

    reply.command_code = EC_FS_CC_DONE;
    reply.return_code = EC_FS_RC_OK;
    fs_reply(c, &reply, sizeof(reply));
    return;
}
