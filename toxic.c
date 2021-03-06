/*  toxic.c
 *
 *
 *  Copyright (C) 2014 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic.
 *
 *  Toxic is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Toxic is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Toxic.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <curses.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <locale.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <getopt.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <limits.h>
#include <termios.h>
#include <ctype.h>
#include <string.h>
#include <curl/curl.h>
#include <tox/tox.h>
#include <tox/toxencryptsave.h>
#include "configdir.h"
#include "toxic.h"
#include "windows.h"
#include "friendlist.h"
#include "prompt.h"
#include "misc_tools.h"
#include "file_transfers.h"
#include "line_info.h"
#include "settings.h"
#include "log.h"
#include "notify.h"
#include "audio_device.h"
#include "message_queue.h"
#include "execute.h"
#include "term_mplex.h"
#include "name_lookup.h"
#include "bootstrap.h"
#include "global_commands.h"
#ifdef X11
#include "xtra.h"
#endif

#ifdef AUDIO
#include "audio_call.h"
#ifdef VIDEO
#include "video_call.h"
#endif /* VIDEO */
ToxAV *av;
#endif /* AUDIO */

#ifdef PYTHON
#include "api.h"
#include "python_api.h"
#endif

#ifndef PACKAGE_DATADIR
#define PACKAGE_DATADIR "."
#endif

#define KEY_LIST    0xE7
#define KEY_ADD     0x7D
#define KEY_DEL     0x7B
#define KEY_ENTER   0x77
#define KEY_A       0x7E
#define KEY_CANCEL  0xB7
//#define KEY_MYID    0xE7

/* Export for use in Callbacks */
char *DATA_FILE = NULL;
char *BLOCK_FILE = NULL;
ToxWindow *prompt = NULL;

#define DATANAME  "toxic_profile.tox"
#define BLOCKNAME "toxic_blocklist"

#define AUTOSAVE_FREQ 600
#define MIN_PASSWORD_LEN 6
#define MAX_PASSWORD_LEN 64



struct Winthread Winthread;
struct cqueue_thread cqueue_thread;
struct av_thread av_thread;
struct arg_opts arg_opts;
struct user_settings *user_settings = NULL;
//char * utilmenuitems[8] = {"apple","book","cat","Dog","egg","fan","glue","hog"};
bool datasaved;
int retval=0;
static struct user_password {
    bool data_is_encrypted;
    char pass[MAX_PASSWORD_LEN + 1];
    int len;
} user_password;

static time_t last_signal_time;

static void catch_SIGINT(int sig)
{
    time_t cur_time = get_unix_time();

    if (difftime(cur_time, last_signal_time) <= 1) {
        Winthread.sig_exit_toxic = 1;
    } else {
        last_signal_time = cur_time;
    }
}

static void catch_SIGSEGV(int sig)
{
    freopen("/dev/tty", "w", stderr);    // make sure stderr is enabled since we may have disabled it
    endwin();
    fprintf(stderr, "Caught SIGSEGV: Aborting toxic session.\n");
    exit(EXIT_FAILURE);
}

static void flag_window_resize(int sig)
{
    Winthread.flag_resize = 1;
}

static void init_signal_catchers(void)
{
    signal(SIGWINCH, flag_window_resize);
    signal(SIGINT, catch_SIGINT);
    signal(SIGSEGV, catch_SIGSEGV);
}

void free_global_data(void)
{
    if (DATA_FILE) {
        free(DATA_FILE);
        DATA_FILE = NULL;
    }

    if (BLOCK_FILE) {
        free(BLOCK_FILE);
        BLOCK_FILE = NULL;
    }

    if (user_settings) {
        free(user_settings);
        user_settings = NULL;
    }
}

void exit_toxic_success(Tox *m)
{
    store_data(m, DATA_FILE);
    memset(&user_password, 0, sizeof(struct user_password));
    kill_all_file_transfers(m);
    kill_all_windows(m);
    terminate_notify();

#ifdef AUDIO
#ifdef VIDEO
    terminate_video();
#endif /* VIDEO */
    terminate_audio();
#endif /* AUDIO */

#ifdef PYTHON
    terminate_python();
#endif /* PYTHON */

    free_global_data();
    tox_kill(m);
    endwin();
    curl_global_cleanup();

#ifdef X11
    /* We have to terminate xtra last coz reasons
     * Please don't call this anywhere else coz trust me
     */
    terminate_xtra();
#endif /* X11 */

    exit(EXIT_SUCCESS);
}

void exit_toxic_err(const char *errmsg, int errcode)
{
    free_global_data();
    freopen("/dev/tty", "w", stderr);
    endwin();
    fprintf(stderr, "Toxic session aborted with error code %d (%s)\n", errcode, errmsg);
    exit(EXIT_FAILURE);
}

static void init_term(void)
{
#if HAVE_WIDECHAR

    if (!arg_opts.default_locale) {
        if (setlocale(LC_ALL, "") == NULL)
            exit_toxic_err("Could not set your locale, please check your locale settings or "
                           "disable unicode support with the -d flag.", FATALERR_LOCALE_NOT_SET);
    }

#endif

    //initscr();
    //cbreak();
    //keypad(stdscr, 1);
    //noecho();
    nonl();
    timeout(100);

    if (has_colors()) {
        short bg_color = COLOR_BLACK;
        start_color();

        if (user_settings->colour_theme == NATIVE_COLS) {
            if (assume_default_colors(-1, -1) == OK)
                bg_color = -1;
        }

        init_pair(0, COLOR_WHITE, COLOR_BLACK);
        init_pair(1, COLOR_GREEN, bg_color);
        init_pair(2, COLOR_CYAN, bg_color);
        init_pair(3, COLOR_RED, bg_color);
        init_pair(4, COLOR_BLUE, bg_color);
        init_pair(5, COLOR_YELLOW, bg_color);
        init_pair(6, COLOR_MAGENTA, bg_color);
        init_pair(7, COLOR_BLACK, COLOR_BLACK);
        init_pair(8, COLOR_BLACK, COLOR_WHITE);
    }

    refresh();
}

static struct _init_messages {
    char **msgs;
    int num;
} init_messages;

/* One-time queue for messages created during init. Do not use after program init. */
static void queue_init_message(const char *msg, ...)
{
    char frmt_msg[MAX_STR_SIZE] = {0};

    va_list args;
    va_start(args, msg);
    vsnprintf(frmt_msg, sizeof(frmt_msg), msg, args);
    va_end(args);

    int i = init_messages.num;
    ++init_messages.num;

    char **new_msgs = realloc(init_messages.msgs, sizeof(char *) * init_messages.num);

    if (new_msgs == NULL)
        exit_toxic_err("Failed in queue_init_message", FATALERR_MEMORY);

    new_msgs[i] = malloc(MAX_STR_SIZE);

    if (new_msgs[i] == NULL)
        exit_toxic_err("Failed in queue_init_message", FATALERR_MEMORY);

    snprintf(new_msgs[i], MAX_STR_SIZE, "%s", frmt_msg);
    init_messages.msgs = new_msgs;
}

/* called after messages have been printed to prompt and are no longer needed */
static void cleanup_init_messages(void)
{
    if (init_messages.num <= 0)
        return;

    int i;

    for (i = 0; i < init_messages.num; ++i)
        free(init_messages.msgs[i]);

    free(init_messages.msgs);
}

static void print_init_messages(ToxWindow *toxwin)
{
    int i;

    for (i = 0; i < init_messages.num; ++i)
        line_info_add(toxwin, NULL, NULL, NULL, SYS_MSG, 0, 0, init_messages.msgs[i]);
}

static void load_friendlist(Tox *m)
{
    size_t i;
    size_t numfriends = tox_self_get_friend_list_size(m);

    for (i = 0; i < numfriends; ++i)
        friendlist_onFriendAdded(NULL, m, i, false);

    sort_friendlist_index();
}

/* return length of password on success, 0 on failure */
static int password_prompt(char *buf, int size)
{
    buf[0] = '\0';

    /* disable terminal echo */
    struct termios oflags, nflags;
    tcgetattr(fileno(stdin), &oflags);
    nflags = oflags;
    nflags.c_lflag &= ~ECHO;
    nflags.c_lflag |= ECHONL;

    if (tcsetattr(fileno(stdin), TCSANOW, &nflags) != 0)
        return 0;

    const char *p = fgets(buf, size, stdin);
    int len = strlen(buf);

    /* re-enable terminal echo */
    tcsetattr(fileno(stdin), TCSANOW, &oflags);

    if (p == NULL || len <= 1)
        return 0;

    /* eat overflowed stdin and return error */
    if (buf[--len] != '\n') {
        int ch;

        while ((ch = getchar()) != '\n' && ch > 0)
            ;

        return 0;
    }

    buf[len] = '\0';
    return len;
}

/* Get the password from the eval command.
 * return length of password on success, 0 on failure
 */
static int password_eval(char *buf, int size)
{
    buf[0] = '\0';

    /* Run password_eval command */
    FILE *f = popen(user_settings->password_eval, "r");

    if (f == NULL) {
        fprintf(stderr, "Executing password_eval failed\n");
        return 0;
    }

    /* Get output from command */
    char *ret = fgets(buf, size, f);

    if (ret == NULL) {
        fprintf(stderr, "Reading password from password_eval command failed\n");
        pclose(f);
        return 0;
    }

    /* Get exit status */
    int status = pclose(f);

    if (status != 0) {
        fprintf(stderr, "password_eval command returned error %d\n", status);
        return 0;
    }

    /* Removez whitespace or \n at end */
    int i, len = strlen(buf);

    for (i = len - 1; i > 0 && isspace(buf[i]); i--) {
        buf[i] = 0;
        len--;
    }

    return len;
}

/* Ask user if they would like to encrypt the data file and set password */
static void first_time_encrypt(const char *msg)
{
    char ch[256] = {0};

    do {
        system("clear");
        printf("%s ", msg);
        fflush(stdout);

        if (!strcasecmp(ch, "y\n") || !strcasecmp(ch, "n\n") || !strcasecmp(ch, "yes\n")
                || !strcasecmp(ch, "no\n") || !strcasecmp(ch, "q\n"))
            break;

    } while (fgets(ch, sizeof(ch), stdin));

    printf("\n");

    if (ch[0] == 'q' || ch[0] == 'Q')
        exit(0);

    if (ch[0] == 'y' || ch[0] == 'Y') {
        int len = 0;
        bool valid_password = false;
        char passconfirm[MAX_PASSWORD_LEN + 1] = {0};

        printf("Enter a new password (must be at least %d characters) ", MIN_PASSWORD_LEN);

        while (valid_password == false) {
            fflush(stdout); // Flush all before user input
            len = password_prompt(user_password.pass, sizeof(user_password.pass));
            user_password.len = len;

            if (strcasecmp(user_password.pass, "q") == 0)
                exit(0);

            if (string_is_empty(passconfirm) && (len < MIN_PASSWORD_LEN || len > MAX_PASSWORD_LEN)) {
                printf("Password must be between %d and %d characters long. ", MIN_PASSWORD_LEN, MAX_PASSWORD_LEN);
                continue;
            }

            if (string_is_empty(passconfirm)) {
                printf("Enter password again ");
                snprintf(passconfirm, sizeof(passconfirm), "%s", user_password.pass);
                continue;
            }

            if (strcmp(user_password.pass, passconfirm) != 0) {
                memset(passconfirm, 0, sizeof(passconfirm));
                memset(user_password.pass, 0, sizeof(user_password.pass));
                printf("Passwords don't match. Try again. ");
                continue;
            }

            valid_password = true;
        }

        queue_init_message("Data file '%s' is encrypted", DATA_FILE);
        memset(passconfirm, 0, sizeof(passconfirm));
        user_password.data_is_encrypted = true;
    }

    system("clear");
}

/* Store Tox profile data to path.
 *
 * Return 0 if stored successfully.
 * Return -1 on error.
 */
#define TEMP_PROFILE_EXT ".tmp"
int store_data(Tox *m, const char *path)
{
    if (path == NULL) {
        return -1;
    }

    char temp_path[strlen(path) + strlen(TEMP_PROFILE_EXT) + 1];
    snprintf(temp_path, sizeof(temp_path), "%s%s", path, TEMP_PROFILE_EXT);

    FILE *fp = fopen(temp_path, "wb");

    if (fp == NULL) {
        return -1;
    }

    size_t data_len = tox_get_savedata_size(m);
    char *data = malloc(data_len * sizeof(char));

    if (data == NULL) {
        fclose(fp);
        return -1;
    }

    tox_get_savedata(m, (uint8_t *) data);

    if (user_password.data_is_encrypted && !arg_opts.unencrypt_data) {
        size_t enc_len = data_len + TOX_PASS_ENCRYPTION_EXTRA_LENGTH;
        char *enc_data = malloc(enc_len * sizeof(char));

        if (enc_data == NULL) {
            fclose(fp);
            free(data);
            return -1;
        }

        TOX_ERR_ENCRYPTION err;
        tox_pass_encrypt((uint8_t *) data, data_len, (uint8_t *) user_password.pass, user_password.len,
                         (uint8_t *) enc_data, &err);

        if (err != TOX_ERR_ENCRYPTION_OK) {
            fprintf(stderr, "tox_pass_encrypt() failed with error %d\n", err);
            fclose(fp);
            free(data);
            free(enc_data);
            return -1;
        }

        if (fwrite(enc_data, enc_len, 1, fp) != 1) {
            fprintf(stderr, "Failed to write profile data.\n");
            fclose(fp);
            free(data);
            free(enc_data);
            return -1;
        }

        free(enc_data);
    } else {  /* data will not be encrypted */
        if (fwrite(data, data_len, 1, fp) != 1) {
            fprintf(stderr, "Failed to write profile data.\n");
            fclose(fp);
            free(data);
            return -1;
        }
    }

    fclose(fp);
    free(data);

    if (rename(temp_path, path) != 0) {
        return -1;
    }

    return 0;
}

static void init_tox_callbacks(Tox *m)
{
    tox_callback_self_connection_status(m, prompt_onSelfConnectionChange);
    tox_callback_friend_connection_status(m, on_connectionchange);
    tox_callback_friend_typing(m, on_typing_change);
    tox_callback_friend_request(m, on_request);
    tox_callback_friend_message(m, on_message);
    tox_callback_friend_name(m, on_nickchange);
    tox_callback_friend_status(m, on_statuschange);
    tox_callback_friend_status_message(m, on_statusmessagechange);
    tox_callback_friend_read_receipt(m, on_read_receipt);
    tox_callback_conference_invite(m, on_groupinvite);
    tox_callback_conference_message(m, on_groupmessage);
    tox_callback_conference_namelist_change(m, on_group_namelistchange);
    tox_callback_conference_title(m, on_group_titlechange);
    tox_callback_file_recv(m, on_file_recv);
    tox_callback_file_chunk_request(m, on_file_chunk_request);
    tox_callback_file_recv_control(m, on_file_control);
    tox_callback_file_recv_chunk(m, on_file_recv_chunk);
}

static void init_tox_options(struct Tox_Options *tox_opts)
{
    tox_options_default(tox_opts);

    tox_opts->ipv6_enabled = !arg_opts.use_ipv4;
    tox_opts->udp_enabled = !arg_opts.force_tcp;
    tox_opts->proxy_type = arg_opts.proxy_type;
    tox_opts->tcp_port = arg_opts.tcp_port;

    if (!tox_opts->ipv6_enabled)
        queue_init_message("Forcing IPv4 connection");

    if (tox_opts->tcp_port)
        queue_init_message("TCP relaying enabled on port %d", tox_opts->tcp_port);

    if (tox_opts->proxy_type != TOX_PROXY_TYPE_NONE) {
        tox_opts->proxy_port = arg_opts.proxy_port;
        tox_opts->proxy_host = arg_opts.proxy_address;
        const char *ps = tox_opts->proxy_type == TOX_PROXY_TYPE_SOCKS5 ? "SOCKS5" : "HTTP";

        char tmp[sizeof(arg_opts.proxy_address) + MAX_STR_SIZE];
        snprintf(tmp, sizeof(tmp), "Using %s proxy %s : %d", ps, arg_opts.proxy_address, arg_opts.proxy_port);
        queue_init_message("%s", tmp);
    }

    if (!tox_opts->udp_enabled) {
        queue_init_message("UDP disabled");
    } else if (tox_opts->proxy_type != TOX_PROXY_TYPE_NONE) {
        const char *msg = "WARNING: Using a proxy without disabling UDP may leak your real IP address.";
        queue_init_message("%s", msg);
        msg = "Use the -t option to disable UDP.";
        queue_init_message("%s", msg);
    }
}

/* Returns a new Tox object on success.
 * If object fails to initialize the toxic process will terminate.
 */
static Tox *load_tox(char *data_path, struct Tox_Options *tox_opts, TOX_ERR_NEW *new_err)
{
    Tox *m = NULL;

    FILE *fp = fopen(data_path, "rb");

    if (fp != NULL) {   /* Data file exists */
        off_t len = file_size(data_path);

        if (len == 0) {
            fclose(fp);
            exit_toxic_err("failed in load_tox", FATALERR_FILEOP);
        }

        char data[len];

        if (fread(data, sizeof(data), 1, fp) != 1) {
            fclose(fp);
            exit_toxic_err("failed in load_tox", FATALERR_FILEOP);
        }

        bool is_encrypted = tox_is_data_encrypted((uint8_t *) data);

        /* attempt to encrypt an already encrypted data file */
        if (arg_opts.encrypt_data && is_encrypted) {
            fclose(fp);
            exit_toxic_err("failed in load_tox", FATALERR_ENCRYPT);
        }

        if (arg_opts.unencrypt_data && is_encrypted)
            queue_init_message("Data file '%s' has been unencrypted", data_path);
        else if (arg_opts.unencrypt_data)
            queue_init_message("Warning: passed --unencrypt-data option with unencrypted data file '%s'", data_path);

        if (is_encrypted) {
            if (!arg_opts.unencrypt_data)
                user_password.data_is_encrypted = true;

            size_t pwlen = 0;
            int pweval = user_settings->password_eval[0];

            if (!pweval) {
                system("clear");   // TODO: is this portable?
                printf("Enter password (q to quit) ");
            }

            size_t plain_len = len - TOX_PASS_ENCRYPTION_EXTRA_LENGTH;
            char plain[plain_len];

            while (true) {
                fflush(stdout); // Flush before prompts so the user sees the question/message

                if (pweval) {
                    pwlen = password_eval(user_password.pass, sizeof(user_password.pass));
                } else {
                    pwlen = password_prompt(user_password.pass, sizeof(user_password.pass));
                }

                user_password.len = pwlen;

                if (strcasecmp(user_password.pass, "q") == 0) {
                    fclose(fp);
                    exit(0);
                }

                if (pwlen < MIN_PASSWORD_LEN) {
                    system("clear");
                    sleep(1);
                    printf("Invalid password. Try again. ");
                    pweval = 0;
                    continue;
                }

                TOX_ERR_DECRYPTION pwerr;
                tox_pass_decrypt((uint8_t *) data, len, (uint8_t *) user_password.pass, pwlen,
                                 (uint8_t *) plain, &pwerr);

                if (pwerr == TOX_ERR_DECRYPTION_OK) {
                    tox_opts->savedata_type = TOX_SAVEDATA_TYPE_TOX_SAVE;
                    tox_opts->savedata_data = (uint8_t *) plain;
                    tox_opts->savedata_length = plain_len;

                    m = tox_new(tox_opts, new_err);

                    if (m == NULL) {
                        fclose(fp);
                        return NULL;
                    }

                    break;
                } else if (pwerr == TOX_ERR_DECRYPTION_FAILED) {
                    system("clear");
                    sleep(1);
                    printf("Invalid password. Try again. ");
                    pweval = 0;
                } else {
                    fclose(fp);
                    exit_toxic_err("tox_pass_decrypt() failed", pwerr);
                }
            }
        } else {   /* data is not encrypted */
            tox_opts->savedata_type = TOX_SAVEDATA_TYPE_TOX_SAVE;
            tox_opts->savedata_data = (uint8_t *) data;
            tox_opts->savedata_length = len;

            m = tox_new(tox_opts, new_err);

            if (m == NULL) {
                fclose(fp);
                return NULL;
            }
        }

        fclose(fp);
    } else {   /* Data file does not/should not exist */
        if (file_exists(data_path))
            exit_toxic_err("failed in load_tox", FATALERR_FILEOP);

        tox_opts->savedata_type = TOX_SAVEDATA_TYPE_NONE;

        m = tox_new(tox_opts, new_err);

        if (m == NULL)
            return NULL;

        if (store_data(m, data_path) == -1)
            exit_toxic_err("failed in load_tox", FATALERR_FILEOP);
    }

    return m;
}

static Tox *load_toxic(char *data_path)
{
    struct Tox_Options tox_opts;
    init_tox_options(&tox_opts);

    TOX_ERR_NEW new_err;
    Tox *m = load_tox(data_path, &tox_opts, &new_err);

    if (new_err == TOX_ERR_NEW_PORT_ALLOC && tox_opts.ipv6_enabled) {
        queue_init_message("Falling back to ipv4");
        tox_opts.ipv6_enabled = false;
        m = load_tox(data_path, &tox_opts, &new_err);
    }

    if (!m)
        exit_toxic_err("tox_new returned fatal error", new_err);

    if (new_err != TOX_ERR_NEW_OK)
        queue_init_message("tox_new returned non-fatal error %d", new_err);

    init_tox_callbacks(m);
    load_friendlist(m);
    load_blocklist(BLOCK_FILE);

    if (tox_self_get_name_size(m) == 0)
        tox_self_set_name(m, (uint8_t *) "Toxic User", strlen("Toxic User"), NULL);

    return m;
}

static void do_toxic(Tox *m)
{
    pthread_mutex_lock(&Winthread.lock);

    if (arg_opts.no_connect) {
        pthread_mutex_unlock(&Winthread.lock);
        return;
    }

    tox_iterate(m, NULL);
    do_tox_connection(m);
    pthread_mutex_unlock(&Winthread.lock);
}

#define INACTIVE_WIN_REFRESH_RATE 10

void *thread_winref(void *data)
{
    Tox *m = (Tox *) data;

    uint8_t draw_count = 0;
    init_signal_catchers();

    while (true) {
        draw_active_window(m);
        draw_count++;

        if (Winthread.flag_resize) {
            on_window_resize();
            Winthread.flag_resize = 0;
        } else if (draw_count >= INACTIVE_WIN_REFRESH_RATE) {
            refresh_inactive_windows();
            draw_count = 0;
        }

        if (Winthread.sig_exit_toxic) {
            pthread_mutex_lock(&Winthread.lock);
            exit_toxic_success(m);
        }
    }
}

void *thread_cqueue(void *data)
{
    Tox *m = (Tox *) data;
    TOX_ERR_FRIEND_QUERY *error;
    int try=1;
    int i;
    while (true) {
        //pthread_mutex_lock(&Winthread.lock);
		

            if(try && (tox_friend_get_connection_status(m, 0, error) != TOX_CONNECTION_NONE))
            {
           printf(" \n in from thread cqueue\n");
           //scanf("%d",&i);    try=0;
           //cmd_call(NULL, NULL, m, 0, 0);
        }

       // pthread_mutex_unlock(&Winthread.lock);

        usleep(4000);
    }
}

#ifdef AUDIO
void *thread_av(void *data)
{
    ToxAV *av = (ToxAV *) data;

    while (true) {
        pthread_mutex_lock(&Winthread.lock);
        toxav_iterate(av);
        pthread_mutex_unlock(&Winthread.lock);

        usleep(toxav_iteration_interval(av) * 1000);
    }
}
#endif  /* AUDIO */

static void print_usage(void)
{
    fprintf(stderr, "usage: toxic [OPTION] [FILE ...]\n");
    fprintf(stderr, "  -4, --ipv4               Force IPv4 connection\n");
    fprintf(stderr, "  -b, --debug              Enable stderr for debugging\n");
    fprintf(stderr, "  -c, --config             Use specified config file\n");
    fprintf(stderr, "  -d, --default-locale     Use default POSIX locale\n");
    fprintf(stderr, "  -e, --encrypt-data       Encrypt an unencrypted data file\n");
    fprintf(stderr, "  -f, --file               Use specified data file\n");
    fprintf(stderr, "  -h, --help               Show this message and exit\n");
    fprintf(stderr, "  -n, --nodes              Use specified DHTnodes file\n");
    fprintf(stderr, "  -o, --noconnect          Do not connect to the DHT network\n");
    fprintf(stderr, "  -p, --SOCKS5-proxy       Use SOCKS5 proxy: Requires [IP] [port]\n");
    fprintf(stderr, "  -P, --HTTP-proxy         Use HTTP proxy: Requires [IP] [port]\n");
    fprintf(stderr, "  -r, --namelist           Use specified name lookup server list\n");
    fprintf(stderr, "  -t, --force-tcp          Force toxic to use a TCP connection (use with proxies)\n");
    fprintf(stderr, "  -T, --tcp-server         Act as a TCP relay server: Requires [port]\n");
    fprintf(stderr, "  -u, --unencrypt-data     Unencrypt an encrypted data file\n");
    fprintf(stderr, "  -v, --version            Print the version\n");
}

static void print_version(void)
{
    fprintf(stderr, "Toxic version %s\n", TOXICVER);
    fprintf(stderr, "Toxcore version %d.%d.%d\n", tox_version_major(), tox_version_minor(), tox_version_patch());
}

static void set_default_opts(void)
{
    memset(&arg_opts, 0, sizeof(struct arg_opts));

    /* set any non-zero defaults here*/
    arg_opts.proxy_type = TOX_PROXY_TYPE_NONE;
}

static void parse_args(int argc, char *argv[])
{
    set_default_opts();

    static struct option long_opts[] = {
        {"file", required_argument, 0, 'f'},
        {"ipv4", no_argument, 0, '4'},
        {"debug", no_argument, 0, 'b'},
        {"default-locale", no_argument, 0, 'd'},
        {"config", required_argument, 0, 'c'},
        {"encrypt-data", no_argument, 0, 'e'},
        {"nodes", required_argument, 0, 'n'},
        {"help", no_argument, 0, 'h'},
        {"noconnect", no_argument, 0, 'o'},
        {"namelist", required_argument, 0, 'r'},
        {"force-tcp", no_argument, 0, 't'},
        {"tcp-server", required_argument, 0, 'T'},
        {"SOCKS5-proxy", required_argument, 0, 'p'},
        {"HTTP-proxy", required_argument, 0, 'P'},
        {"unencrypt-data", no_argument, 0, 'u'},
        {"version", no_argument, 0, 'v'},
        {NULL, no_argument, NULL, 0},
    };

    const char *opts_str = "4bdehotuxvc:f:n:r:p:P:T:";
    int opt, indexptr;
    long int port = 0;

    while ((opt = getopt_long(argc, argv, opts_str, long_opts, &indexptr)) != -1) {
        switch (opt) {
            case '4':
                arg_opts.use_ipv4 = 1;
                break;

            case 'b':
                arg_opts.debug = 1;
                queue_init_message("stderr enabled");
                break;

            case 'c':
                snprintf(arg_opts.config_path, sizeof(arg_opts.config_path), "%s", optarg);

                if (!file_exists(arg_opts.config_path))
                    queue_init_message("Config file not found");

                break;

            case 'd':
                arg_opts.default_locale = 1;
                queue_init_message("Using default POSIX locale");
                break;

            case 'e':
                arg_opts.encrypt_data = 1;
                break;

            case 'f':
                arg_opts.use_custom_data = 1;

                if (DATA_FILE)
                    free(DATA_FILE);

                if (BLOCK_FILE)
                    free(BLOCK_FILE);

                DATA_FILE = malloc(strlen(optarg) + 1);
                strcpy(DATA_FILE, optarg);

                if (DATA_FILE == NULL)
                    exit_toxic_err("failed in parse_args", FATALERR_MEMORY);

                BLOCK_FILE = malloc(strlen(optarg) + strlen("-blocklist") + 1);

                if (BLOCK_FILE == NULL)
                    exit_toxic_err("failed in parse_args", FATALERR_MEMORY);

                strcpy(BLOCK_FILE, optarg);
                strcat(BLOCK_FILE, "-blocklist");

                queue_init_message("Using '%s' data file", DATA_FILE);

                break;

            case 'n':
                snprintf(arg_opts.nodes_path, sizeof(arg_opts.nodes_path), "%s", optarg);
                break;

            case 'o':
                arg_opts.no_connect = 1;
                queue_init_message("DHT disabled");
                break;

            case 'p':
                arg_opts.proxy_type = TOX_PROXY_TYPE_SOCKS5;
                snprintf(arg_opts.proxy_address, sizeof(arg_opts.proxy_address), "%s", optarg);

                if (++optind > argc || argv[optind - 1][0] == '-')
                    exit_toxic_err("Proxy error", FATALERR_PROXY);

                port = strtol(argv[optind - 1], NULL, 10);

                if (port <= 0 || port > MAX_PORT_RANGE)
                    exit_toxic_err("Proxy error", FATALERR_PROXY);

                arg_opts.proxy_port = port;
                break;

            case 'P':
                arg_opts.proxy_type = TOX_PROXY_TYPE_HTTP;
                snprintf(arg_opts.proxy_address, sizeof(arg_opts.proxy_address), "%s", optarg);

                if (++optind > argc || argv[optind - 1][0] == '-')
                    exit_toxic_err("Proxy error", FATALERR_PROXY);

                port = strtol(argv[optind - 1], NULL, 10);

                if (port <= 0 || port > MAX_PORT_RANGE)
                    exit_toxic_err("Proxy error", FATALERR_PROXY);

                arg_opts.proxy_port = port;
                break;

            case 'r':
                snprintf(arg_opts.nameserver_path, sizeof(arg_opts.nameserver_path), "%s", optarg);

                if (!file_exists(arg_opts.nameserver_path))
                    queue_init_message("nameserver list not found");

                break;

            case 't':
                arg_opts.force_tcp = 1;
                break;

            case 'T':
                port = strtol(optarg, NULL, 10);

                if (port <= 0 || port > MAX_PORT_RANGE)
                    port = 14191;

                arg_opts.tcp_port = port;
                break;

            case 'u':
                arg_opts.unencrypt_data = 1;
                break;

            case 'v':
                print_version();
                exit(EXIT_SUCCESS);

            case 'h':
            default:
                print_usage();
                exit(EXIT_SUCCESS);
        }
    }
}

/* Looks for an old default profile data file and blocklist, and renames them to the new default names.
 *
 * Returns 0 on success.
 * Returns -1 on failure.
 */
#define OLD_DATA_NAME "data"
#define OLD_DATA_BLOCKLIST_NAME "data-blocklist"
static int rename_old_profile(const char *user_config_dir)
{
    char old_data_file[strlen(user_config_dir) + strlen(CONFIGDIR) + strlen(OLD_DATA_NAME) + 1];
    snprintf(old_data_file, sizeof(old_data_file), "%s%s%s", user_config_dir, CONFIGDIR, OLD_DATA_NAME);

    if (!file_exists(old_data_file))
        return 0;

    if (file_exists(DATA_FILE))
        return 0;

    if (rename(old_data_file, DATA_FILE) != 0)
        return -1;

    queue_init_message("Data file has been moved to %s", DATA_FILE);

    char old_data_blocklist[strlen(user_config_dir) + strlen(CONFIGDIR) + strlen(OLD_DATA_BLOCKLIST_NAME) + 1];
    snprintf(old_data_blocklist, sizeof(old_data_blocklist), "%s%s%s", user_config_dir, CONFIGDIR, OLD_DATA_BLOCKLIST_NAME);

    if (!file_exists(old_data_blocklist))
        return 0;

    if (file_exists(BLOCK_FILE))
        return 0;

    if (rename(old_data_blocklist, BLOCK_FILE) != 0)
        return -1;

    return 0;
}

/* Initializes the default config directory and data files used by toxic.
 *
 * Exits the process with an error on failure.
 */
static void init_default_data_files(void)
{
    if (arg_opts.use_custom_data)
        return;

    char *user_config_dir = get_user_config_dir();

    if (user_config_dir == NULL)
        exit_toxic_err("failed in init_default_data_files()", FATALERR_FILEOP);

    int config_err = create_user_config_dirs(user_config_dir);

    if (config_err == -1) {
        DATA_FILE = strdup(DATANAME);
        BLOCK_FILE = strdup(BLOCKNAME);

        if (DATA_FILE == NULL || BLOCK_FILE == NULL)
            exit_toxic_err("failed in init_default_data_files()", FATALERR_MEMORY);
    } else {
        DATA_FILE = malloc(strlen(user_config_dir) + strlen(CONFIGDIR) + strlen(DATANAME) + 1);
        BLOCK_FILE = malloc(strlen(user_config_dir) + strlen(CONFIGDIR) + strlen(BLOCKNAME) + 1);

        if (DATA_FILE == NULL || BLOCK_FILE == NULL)
            exit_toxic_err("failed in init_default_data_files()", FATALERR_MEMORY);

        strcpy(DATA_FILE, user_config_dir);
        strcat(DATA_FILE, CONFIGDIR);
        strcat(DATA_FILE, DATANAME);

        strcpy(BLOCK_FILE, user_config_dir);
        strcat(BLOCK_FILE, CONFIGDIR);
        strcat(BLOCK_FILE, BLOCKNAME);
    }

    /* For backwards compatibility with old toxic profile names. TODO: remove this some day */
    if (rename_old_profile(user_config_dir) == -1)
        queue_init_message("Warning: Profile backwards compatibility failed.");

    free(user_config_dir);
}

// this doesn't do anything (yet)
#ifdef X11
void DnD_callback(const char *asdv, DropType dt)
{
    // if (dt != DT_plain)
    //     return;

    // pthread_mutex_lock(&Winthread.lock);
    // line_info_add(prompt, NULL, NULL, NULL, SYS_MSG, 0, 0, asdv);
    // pthread_mutex_unlock(&Winthread.lock);
}
#endif /* X11 */

//int main(int argc, char **argv)

int main()
{
    int nos=0;int num=0;
    int ret;
    int arg=2,i;
    char (argx[3])[MAX_STR_SIZE];//=NULL;//{"","",""};
    size_t k;
    char keyr,keyret=0xFF;
    char pubkey_buf[TOX_PUBLIC_KEY_SIZE * 2 + 1];
    char lcdbuf[20];
    //char *id; //id[65]
    //id =(unsigned char*)malloc(65);

    //keypad_init();sleep(1);   //we are doing this in module so no needed anymore
    lcdk_open();
    //parse_args(argc, argv);
    extern FriendsList Friends;
    /* Use the -b flag to enable stderr */
    if (!arg_opts.debug)
        freopen("/dev/null", "w", stderr);

    if (arg_opts.encrypt_data && arg_opts.unencrypt_data) {
        arg_opts.encrypt_data = 0;
        arg_opts.unencrypt_data = 0;
        queue_init_message("Warning: Using --unencrypt-data and --encrypt-data simultaneously has no effect");
    }

    /* Make sure all written files are read/writeable only by the current user. */
    umask(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    init_default_data_files();

    bool datafile_exists = file_exists(DATA_FILE);

    if (!datafile_exists && !arg_opts.unencrypt_data)
        first_time_encrypt("Creating new data file. Would you like to encrypt it? Y/n (q to quit)");
    else if (arg_opts.encrypt_data)
        first_time_encrypt("Encrypt existing data file? Y/n (q to quit)");


    /* init user_settings struct and load settings from conf file */
    user_settings = calloc(1, sizeof(struct user_settings));

    if (user_settings == NULL)
        exit_toxic_err("failed in main", FATALERR_MEMORY);

    const char *p = arg_opts.config_path[0] ? arg_opts.config_path : NULL;

    if (settings_load(user_settings, p) == -1) {
        queue_init_message("Failed to load user settings");
    }

    int curl_init = curl_global_init(CURL_GLOBAL_ALL);
    int nameserver_ret = name_lookup_init(curl_init);

    if (nameserver_ret == -1) {
        queue_init_message("curl failed to initialize; name lookup service is disabled.");
    } else if (nameserver_ret == -2) {
        queue_init_message("Name lookup server list could not be found.");
    } else if (nameserver_ret == -3) {
        queue_init_message("Name lookup server list does not contain any valid entries.");
    }

#ifdef X11

    if (init_xtra(DnD_callback) == -1)
        queue_init_message("X failed to initialize");

#endif

    Tox *m = load_toxic(DATA_FILE);

    if (arg_opts.encrypt_data && !datafile_exists)
        arg_opts.encrypt_data = 0;


    init_term();


    /* thread for ncurses stuff */
    if (pthread_mutex_init(&Winthread.lock, NULL) != 0)
        exit_toxic_err("failed in main", FATALERR_MUTEX_INIT);


    /* thread for message queue */
    /* if (pthread_create(&cqueue_thread.tid, NULL, thread_cqueue, (void *) m) != 0)
        exit_toxic_err("failed in main", FATALERR_THREAD_CREATE); */

#ifdef AUDIO

    av = init_audio(prompt, m);

#ifdef VIDEO
    init_video(prompt, m);

#endif /* VIDEO */

    /* AV thread */
    if (pthread_create(&av_thread.tid, NULL, thread_av, (void *) av) != 0)
        exit_toxic_err("failed in main", FATALERR_THREAD_CREATE);

    set_primary_device(input, user_settings->audio_in_dev);
    set_primary_device(output, user_settings->audio_out_dev);

#elif SOUND_NOTIFY

    if ( init_devices() == de_InternalError )
        queue_init_message("Failed to init audio devices");

#endif /* AUDIO */

#ifdef PYTHON

    init_python(m);
    invoke_autoruns(prompt->chatwin->history, prompt);

#endif /* PYTHON */

    init_notify(60, 3000);

    /* screen/tmux auto-away timer */
    if (init_mplex_away_timer(m) == -1)
        queue_init_message("Failed to init mplex auto-away.");

    int nodeslist_ret = load_DHT_nodeslist();

    if (nodeslist_ret != 0) {
        queue_init_message("DHT nodeslist failed to load (error %d)", nodeslist_ret);
    }

    pthread_mutex_lock(&Winthread.lock);
    print_init_messages(prompt);
    pthread_mutex_unlock(&Winthread.lock);

    cleanup_init_messages();

    display_menu(&keyret);
    num=Friends.num_friends;


    do{

    //display_menu(&keyret);
    //num=Friends.num_friends;

    switch (keyret) {
                    case KEY_LIST:
                                  if(num==0){
                                      lcdk_dispclr();
                                      lcdk_disptext(1,0,"No contacts available",0);
                                      lcdk_disptext(2,0,"To add new contacts");
                                      lcdk_disptext(3,0,"press add button");

                                      sleep(3);
                                  }
                                  else if(num>0){
                                      lcdk_dispclr();
                                      lcdk_disptext(2,0,"Loading contacts..",0);
                                      sleep(1);
                                      // cmd_myid(NULL, NULL, m, 0, NULL);
                                      datasaved=returnstatus();
                                      printf("datasaved= %d\n",datasaved);
                                      open_contacts();
                                      if(datasaved)
                                      restore_contacts();
                                      do{
                                      retval=list_contacts();
                                      printf("retval =%d\n",retval);

                                    } while(retval>0);

                                      close_contacts();
                                  }
                                  //close_contacts();
                                  //restore_contacts();
                                  break;
                           case KEY_ADD:
                                     //open_contacts();
                                     lcdk_dispclr();
                                     lcdk_disptext(1,0,"Enter new Id to add",0);
                                     sleep(1);
                                     //printf("id addre =%p\n",pubkey_buf);
                                     ret=lcdk_getid(argx[1]);
                                     if(ret){
                                     printf("id= %s\n",argx[1]);
                                     cmd_add(NULL, NULL, m, arg, argx);
                                     lcdk_dispclr();
                                     lcdk_disptext(1,0,"Enter Name",0);
                                     lcdk_getname(argx[2]);
                                     friendlist_NickChange(NULL,m, nos, argx[2],20);
                                     open_contacts();
                                     //by this time both name and key both are filled in friend structure so we are saving them to retrieve later
                                     save_contact(nos,&Friends.list[nos].name,&Friends.list[nos].pub_key);
                                     savestatus(1);
                                     close_contacts();
                                     }
                                     else{
                                         lcdk_dispclr();
                                         lcdk_disptext(1,0,"Cancelled by User ..",0);
                                         sleep(2);//sleep is to make user observe displayed message on screen
                                     }
                                     break;
                            case KEY_DEL:
                                     lcdk_dispclr();
                                     lcdk_disptext(1,0,"Enter number to delete",0);
                                     //for now we are deleting all contacts for testing
                                     for(i=0;i<num;i++){
                                     block_friend(m, (uint32_t) i);
                                     }
                                     savestatus(0);
                                     num=Friends.num_friends;
                                     printf("num of friends after delete call =%d\n",num);
                                     break;
                           /* case KEY_ENTER:
                                     break;  */
                            case KEY_CANCEL:
                                    break;
                            case KEY_ENTER:
                                    lcdk_dispclr();
                                    sleep(1);
                                    memset(pubkey_buf,0,TOX_PUBLIC_KEY_SIZE * 2 + 1);
                                    cmd_myid(NULL, NULL, m, 0, pubkey_buf);
                                    pubkey_buf[65]='\0';
                                    for(i=0;i<=3;i++){
                                    memset(lcdbuf,0,20);
                                    sleep(1);
                                    memcpy(lcdbuf,pubkey_buf+i*19,19);
                                    lcdbuf[19]='\0';
                                    lcdk_disptext(i,0,lcdbuf,0);
                                    }
                                    do{
                                    keypadread(&keyr);
                                    }while(keyr!=KEY_CANCEL);
                                    break;
                                
                            default:
                                    break;
    }

    display_menu(&keyret);
    printf("keyret prior to while check= %x \n",keyret);
    if(keyret== KEY_CANCEL)
    {
        lcdk_dispclr();
        lcdk_disptext(1,0,"Exiting from menu options",0);
        sleep(2);
    }



    } while(keyret!=KEY_CANCEL);//(retval != -2||keyret!=0xB7); //-2 is returned once entered contact list and none selected, KEY_A is returned in the start
    //if none is selected
   //here we need to be care full that there are two different values are returned those are retval and keyret we need to observe and use them properly
 // otherwise while loop breaking is not happening.and code is not working in an intended way


    // cmd_myid(NULL, NULL, m, 0, NULL);


    //    size_t       i, ii;
    //    FriendsList  friends;
    /*    char pubkey_buf[TOX_PUBLIC_KEY_SIZE * 2 + 1];
    TOX_ERR_FRIEND_QUERY *err;
    //    friends = api_get_friendslist();

    // display_friends();


    for (i = 0; i <=Friends.num_friends; i++) { printf("IN FOR loop list of friends %d\n",i);
        for (ii = 0; ii < TOX_PUBLIC_KEY_SIZE; ii++)
            snprintf(pubkey_buf + ii * 2, 3, "%02X", Friends.list[i].pub_key[ii] & 0xff);

        pubkey_buf[TOX_PUBLIC_KEY_SIZE * 2] = '\0';


     printf("\n  pubkey buf %s\n",pubkey_buf);


    } */
    //ToxicFriend
    // cmd call shoud be tried in a diff way we need to pass friend_number instead of his pubkey.this is selected from friend list index
    // list index is the param passed after m in the default calls its 0 i.e., very first friend in list
   /*******here too from */
    // do_toxic(m);
   // sleep(5);
/*
    if(TOX_CONNECTION_NONE == tox_friend_get_connection_status(m,0, NULL))
    {
        sleep(2);
        printf("friend is ofline\n");
    }
*/
    /*upto here**/
    //if(tox_friend_get_connection_status(m,0, NULL))
    //cmd_call(NULL, NULL, m, 0, 0);

    //void cmd_call(WINDOW *window, ToxWindow *self, Tox *m, int argc, char (*argv)[MAX_STR_SIZE])


    /*uint32_t i;

    for (i = 0; i <= Friends.max_idx; ++i) {
      //  if (Friends.list[i].active)
        //    continue;

        printf("\n Friend no =%d",Friends.num_friends);//Friends.list[i].num) ;

    }*/



    //for (i = 0; i < TOX_PUBLIC_KEY_SIZE; ++i)
    //      printf( "%02X", Friends.list[selected_num].pub_key[i] & 0xff);


    //execute(prompt->chatwin->history, prompt, m, avatarstr, GLOBAL_COMMAND_MODE);

   /*from here on its to start again*/
  /*  time_t last_save = get_unix_time();
    printf("prior to save data\n");

    while (true){
        do_toxic(m);
        time_t cur_time = get_unix_time();
        //printf("in while\n");
        if (timed_out(last_save, AUTOSAVE_FREQ)) {
            pthread_mutex_lock(&Winthread.lock);

            if (store_data(m, DATA_FILE) != 0){
                line_info_add(prompt, NULL, NULL, NULL, SYS_MSG, 0, RED, "WARNING: Failed to save to data file");
                printf("unable to save data\n");}

            pthread_mutex_unlock(&Winthread.lock);
            printf("after saving the data\n");
            last_save = cur_time;
        }
        usleep(tox_iteration_interval(m) * 1000);
    }

    while(TOX_CONNECTION_NONE == tox_friend_get_connection_status(m,0, NULL))
    {
        sleep(3);
        printf("friend is ofline\n");
    } */
    /*    if(tox_friend_get_connection_status(m,0, NULL))
    cmd_call(NULL, NULL, m, 0, 0);
*/
 lcdk_close();
    return 0;
}

