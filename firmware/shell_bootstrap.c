/*
 * One-shot root-access bootstrap, compiled into libxreal_cpu_adapter.so.
 *
 * Pilot (uid 0) dlopens the adapter at start, so this constructor runs as root
 * once per Pilot start. It only sets up state; it never spawns a process,
 * creates a thread, allocates memory or writes to Pilot's data, so a failure
 * here cannot hang or kill the renderer.
 *
 * Uses only symbols already present in the device ABI
 * (firmware/abi_imports.json: open, read, write, close, rename, snprintf).
 *
 * What it does, all idempotent:
 *   1. replaces the root password hash in /etc/passwd (backup beside it),
 *   2. installs an SSH public key in /etc/dropbear/authorized_keys,
 *   3. renames runit's /etc/services/dropbear/down marker so dropbear also
 *      starts at every boot,
 *   4. writes 'u' into supervise/control so runsv starts dropbear right now.
 *
 * Status goes to /usrdata/shell-bootstrap.log, readable over the USB HID
 * export command, so the result is verifiable before anyone tries SSH.
 */

#define O_RDONLY 0
#define O_WRONLY 01
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_NONBLOCK 04000

extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long count);
extern long write(int fd, const void *buf, unsigned long count);
extern int fsync(int fd);
extern int close(int fd);
extern int rename(const char *oldpath, const char *newpath);

static const char LOG_PATH[] = "/usrdata/shell-bootstrap.log";
static const char PASSWD[] = "/etc/passwd";
static const char PASSWD_NEW[] = "/etc/passwd.hermes.new";
static const char PASSWD_BAK[] = "/etc/passwd.hermes.bak";
static const char AUTH_KEYS[] = "/etc/dropbear/authorized_keys";
static const char RUNIT_DOWN[] = "/etc/services/dropbear/down";
static const char RUNIT_DOWN_OFF[] = "/etc/services/dropbear/down.disabled";
static const char RUNIT_CONTROL[] = "/etc/services/dropbear/supervise/control";

/* MD5-crypt, salt Xr7kQm2p. The stock file already uses $1$, so this needs no
 * crypt feature that the device lacks. Change it together with the salt. */
static const char ROOT_HASH[] = "$1$Xr7kQm2p$bCxzH1op71tPwoe.qBNMI0";
static const char PUBKEY[] =
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBxruxl4EEnfBKcAN4XwkuQGTpwQAUs613eNiYs2u+TJ"
    " hermes-onepro-root\n";

#define PASSWD_MAX 4096
#define OUT_MAX (PASSWD_MAX + 128)

static int log_fd = -1;

/* String lengths are compile-time: sizeof - 1. A runtime strlen loop got folded
 * into a strlen() call by the optimiser and strlen is not in the device ABI. */
#define MSG(s) (s), (sizeof(s) - 1)

static void say(const char *msg, unsigned long len)
{
    if (log_fd < 0)
        return;
    (void)write(log_fd, msg, len);
}

static long read_file(const char *path, char *buf, unsigned long cap)
{
    int fd = open(path, O_RDONLY);
    long total = 0;
    if (fd < 0)
        return -1;
    while ((unsigned long)total < cap) {
        long n = read(fd, buf + total, cap - (unsigned long)total);
        if (n <= 0)
            break;
        total += n;
    }
    close(fd);
    return total;
}

static int write_file(const char *path, const char *data, unsigned long len, int mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    unsigned long off = 0;
    if (fd < 0)
        return -1;
    while (off < len) {
        long n = write(fd, data + off, len - off);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        off += (unsigned long)n;
    }
    if (fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void fix_passwd(void)
{
    static char old[PASSWD_MAX];
    static char neo[OUT_MAX];
    static const char prefix[] = "root:";
    unsigned long hash_len = sizeof(ROOT_HASH) - 1;
    unsigned long i, out_len = 0;
    long n, colon = -1;

    n = read_file(PASSWD, old, PASSWD_MAX);
    if (n <= 0 || n >= PASSWD_MAX) {
        say(MSG("passwd: unreadable or too large\n"));
        return;
    }
    if (n < 6 || old[0] != 'r' || old[1] != 'o' || old[2] != 'o' || old[3] != 't' || old[4] != ':') {
        say(MSG("passwd: no root line at start, left alone\n"));
        return;
    }
    for (long k = 5; k < n; k++) {
        if (old[k] == ':') {
            colon = k;
            break;
        }
        if (old[k] == '\n')
            break;
    }
    if (colon < 0) {
        say(MSG("passwd: malformed root line, left alone\n"));
        return;
    }
    if ((unsigned long)(colon - 5) == hash_len) {
        for (i = 0; i < hash_len && old[5 + i] == ROOT_HASH[i]; i++)
            ;
        if (i == hash_len) {
            say(MSG("passwd: root hash already set\n"));
            return;
        }
    }
    if (write_file(PASSWD_BAK, old, (unsigned long)n, 0600) != 0) {
        say(MSG("passwd: backup failed, left alone\n"));
        return;
    }
    for (i = 0; i < 5; i++)
        neo[out_len++] = prefix[i];
    for (i = 0; i < hash_len; i++)
        neo[out_len++] = ROOT_HASH[i];
    for (long k = colon; k < n; k++)
        neo[out_len++] = old[k];
    if (write_file(PASSWD_NEW, neo, out_len, 0644) != 0) {
        say(MSG("passwd: write failed, left alone\n"));
        return;
    }
    if (rename(PASSWD_NEW, PASSWD) != 0) {
        say(MSG("passwd: rename failed, backup kept\n"));
        return;
    }
    say(MSG("passwd: root hash replaced\n"));
}

static void install_key(void)
{
    static char current[256];
    unsigned long key_len = sizeof(PUBKEY) - 1;
    unsigned long i;
    long n = read_file(AUTH_KEYS, current, sizeof(current));

    if (n == (long)key_len) {
        for (i = 0; i < key_len && current[i] == PUBKEY[i]; i++)
            ;
        if (i == key_len) {
            say(MSG("key: already installed\n"));
            return;
        }
    }
    if (write_file(AUTH_KEYS, PUBKEY, key_len, 0600) != 0) {
        say(MSG("key: write failed\n"));
        return;
    }
    say(MSG("key: installed\n"));
}

static void start_service(void)
{
    int fd;
    long n;

    if (rename(RUNIT_DOWN, RUNIT_DOWN_OFF) == 0)
        say(MSG("runit: down marker disabled (dropbear now starts at boot)\n"));
    else
        say(MSG("runit: no down marker present\n"));

    fd = open(RUNIT_CONTROL, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        say(MSG("runit: control fifo unavailable, service starts next boot\n"));
        return;
    }
    n = write(fd, "u", 1);
    close(fd);
    if (n == 1)
        say(MSG("runit: dropbear start requested\n"));
    else
        say(MSG("runit: control write failed\n"));
}

__attribute__((constructor)) static void hermes_shell_bootstrap(void)
{
    log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    say(MSG("start\n"));
    fix_passwd();
    install_key();
    start_service();
    say(MSG("done\n"));
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
}
