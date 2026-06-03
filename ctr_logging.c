#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Different types of container logging */
static gboolean use_fluentd_logging = FALSE;

/* Value the user must input for each log driver */
static const char *const FLUENTD_STRING = "fluentd";

/* fluentd log parameters */
#define FLUENTD_DEFAULT_ADDRESS "localhost:24224"
#define FLUENTD_DEFAULT_PORT "24224"
#define FLUENTD_MESSAGE_ELEMENTS 3
#define FLUENTD_MIN_MAP_FIELDS 3
static int fluentd_fd = -1;
static char *fluentd_address = NULL;
static char *fluentd_tag = NULL;

static void parse_log_path(char *log_config);
static gboolean parse_fluentd_log_path(const char *log_config);
static int write_fluentd(stdpipe_t pipe, const char *buf, ssize_t buflen);

static void configure_fluentd_logging(char *cuuid_, char *name_, char *tag);
static int connect_fluentd(const char *address);
static int connect_fluentd_tcp(const char *address);
static int connect_fluentd_unix(const char *address);
static int fluentd_send_record(stdpipe_t pipe, const char *log, ssize_t log_len);
static void validate_log_labels(gchar **log_labels);

/*
 * configures container log specific
 * information, such as the drivers the user
 * called with and the max log size for log
 * file types. For the log file types
 * (currently just k8s log file), it will also
 * open the log_fd for that specific log file.
 */
void configure_log_drivers(gchar **log_drivers, int64_t log_size_max_, int64_t log_global_size_max_, char *cuuid_, char *name_, char *tag, gchar **log_labels){
  log_size_max = log_size_max_;
  log_global_size_max = log_global_size_max_;
  if (log_drivers == NULL)
    nexit("Log driver not provided. Use "
          "--log-path");
  for (int driver = 0; log_drivers[driver];
       ++driver)
  {
    parse_log_path(log_drivers[driver]);
  }
  if (use_k8s_logging)
  {
    /* Open the log path file. */
    k8s_log_fd = open(k8s_log_path,
                      O_WRONLY | O_APPEND |
                          O_CREAT | O_CLOEXEC,
                      0640);
    if (k8s_log_fd < 0)
      pexit("Failed to open log file");

    struct stat statbuf;
    if (fstat(k8s_log_fd, &statbuf) == 0)
    {
      k8s_bytes_written = statbuf.st_size;
    }
    else
    {
      nwarnf("Could not stat log file %s, "
             "assuming 0 size",
             k8s_log_path);
      k8s_bytes_written = 0;
    }
    k8s_total_bytes_written =
        k8s_bytes_written;

    if (!use_journald_logging &&
        !use_fluentd_logging)
    {
      if (tag)
      {
        nexit("k8s-file doesn't support "
              "--log-tag");
      }
      if (log_labels)
      {
        nexit("k8s-file doesn't support "
              "--log-label");
      }
    }
  }

  if (use_journald_logging)
  {
#ifndef USE_JOURNALD
    nexit("Include journald in compilation "
          "path to log to systemd journal");
#endif
    /* save the length so we don't have to
     * compute every sd_journal_* call */
    if (cuuid_ == NULL)
      nexit("Container ID must be provided "
            "and of the correct length");
    cuuid_len = strlen(cuuid_);
    if (cuuid_len <= TRUNC_ID_LEN)
      nexit("Container ID must be longer than "
            "12 characters");

    cuuid = cuuid_;
    strncpy(short_cuuid, cuuid, TRUNC_ID_LEN);
    short_cuuid[TRUNC_ID_LEN] = '\0';
    name = name_;

    /* Setup some sd_journal_sendv arguments
     * that won't change */
    container_id_full = g_strdup_printf(
        "CONTAINER_ID_FULL=%s", cuuid);
    container_id = g_strdup_printf(
        "CONTAINER_ID=%s", short_cuuid);

    /* Priority order of syslog_identifier (in
     * order of precedence) is tag, name,
     * `conmon`. */
    syslog_identifier = g_strdup_printf(
        "SYSLOG_IDENTIFIER=%s", short_cuuid);
    syslog_identifier_len =
        TRUNC_ID_LEN +
        SYSLOG_IDENTIFIER_EQ_LEN;
    if (name)
    {
      name_len = strlen(name);
      container_name = g_strdup_printf(
          "CONTAINER_NAME=%s", name);

      g_free(syslog_identifier);
      syslog_identifier = g_strdup_printf(
          "SYSLOG_IDENTIFIER=%s", name);
      syslog_identifier_len =
          name_len + SYSLOG_IDENTIFIER_EQ_LEN;
    }
    if (tag)
    {
      container_tag = g_strdup_printf(
          "CONTAINER_TAG=%s", tag);
      container_tag_len =
          strlen(container_tag);

      g_free(syslog_identifier);
      syslog_identifier = g_strdup_printf(
          "SYSLOG_IDENTIFIER=%s", tag);
      syslog_identifier_len =
          strlen(syslog_identifier);
    }
    if (log_labels)
    {
      container_labels = log_labels;
      validate_log_labels(log_labels);
    }
  }

  if (use_fluentd_logging)
  {
    if (log_labels)
      container_labels = log_labels;
    configure_fluentd_logging(cuuid_, name_,
                              tag);
  }
}

/*
 * parse_log_path branches on log driver type
 * the user inputted. log_config will either be
 * a ':' delimited string containing:
 * <DRIVER_NAME>:<PATH_NAME> or <PATH_NAME>
 * in the case of no colon, the driver will be
 * kubernetes-log-file, in the case the log
 * driver is 'journald', the <PATH_NAME> is
 * ignored. fluentd uses the <PATH_NAME> as its
 * forward protocol address. exits with error
 * if <DRIVER_NAME> isn't a supported log
 * driver.
 */
static void parse_log_path(char *log_config)
{
  const char *delim;
  char *driver;
  char *path;

  if (log_config == NULL ||
      *log_config == '\0')
    nexitf("log-path must not be empty");

  if (parse_fluentd_log_path(log_config))
    return;

  delim = strchr(log_config, ':');
  driver = strtok(log_config, ":");
  path = strtok(NULL, ":");

  if (path == NULL && driver == NULL)
  {
    nexitf("log-path must not be empty");
  }

  // :none is not the same as none, nor is
  // :journald the same as journald we check
  // the delim here though, because we DO want
  // to match "none" as the none driver
  if (path == NULL && delim == log_config)
  {
    path = driver;
    driver = (char *)K8S_FILE_STRING;
  }

  if (!strcmp(driver, "off") ||
      !strcmp(driver, "null") ||
      !strcmp(driver, "none"))
  {
    // no-op, this means things like
    // --log-driver journald --log-driver none
    // will still log to journald.
    return;
  }

  if (!strcmp(driver, "passthrough"))
  {
    use_logging_passthrough = TRUE;
    return;
  }

  if (!strcmp(driver, JOURNALD_FILE_STRING))
  {
    use_journald_logging = TRUE;
    return;
  }

  // Driver is k8s-file or empty
  if (!strcmp(driver, K8S_FILE_STRING))
  {
    if (path == NULL)
    {
      nexitf("k8s-file requires a filename");
    }
    use_k8s_logging = TRUE;
    g_free(k8s_log_path);
    k8s_log_path = g_strdup(path);
    return;
  }

  // If no : was found, use the entire log-path
  // as a filename to k8s-file.
  if (path == NULL && delim == NULL)
  {
    use_k8s_logging = TRUE;
    g_free(k8s_log_path);
    k8s_log_path = g_strdup(driver);
    return;
  }

  nexitf("No such log driver %s", driver);
}

static gboolean parse_fluentd_log_path(const char *log_config){
  const char *address;

  if (log_config == NULL)
    return FALSE;

  if (!strcmp(log_config, FLUENTD_STRING))
  {
    address = FLUENTD_DEFAULT_ADDRESS;
  }
  else if (g_str_has_prefix(log_config,
                            "fluentd:"))
  {
    address = log_config + strlen("fluentd:");
    if (*address == '\0')
      address = FLUENTD_DEFAULT_ADDRESS;
  }
  else
  {
    return FALSE;
  }

  use_fluentd_logging = TRUE;
  g_free(fluentd_address);
  fluentd_address = g_strdup(address);
  return TRUE;
}

static void validate_log_labels(gchar **log_labels){
  if (log_labels == NULL)
    return;

  for (char **ptr = log_labels; *ptr; ptr++)
  {
    if (**ptr == '=')
    {
      nexitf(
          "Container labels must be in format "
          "LABEL=VALUE (no LABEL present "
          "in '%s')",
          *ptr);
    }
    if (count_chars_in_string(*ptr, '=') !=
        1)
    {
      nexitf(
          "Container labels must be in format "
          "LABEL=VALUE (none or more "
          "than one '=' present in '%s')",
          *ptr);
    }
    if (!is_valid_label_name(*ptr))
    {
      nexitf(
          "Container label names must contain "
          "only uppercase letters, "
          "numbers and underscore (in '%s')",
          *ptr);
    }
  }
}

static void configure_fluentd_logging(char *cuuid_, char *name_, char *tag){
  if (cuuid_ == NULL)
    nexit("Container ID must be provided and "
          "of the correct length");

  cuuid_len = strlen(cuuid_);
  if (cuuid_len <= TRUNC_ID_LEN)
    nexit("Container ID must be longer than "
          "12 characters");

  cuuid = cuuid_;
  strncpy(short_cuuid, cuuid, TRUNC_ID_LEN);
  short_cuuid[TRUNC_ID_LEN] = '\0';

  name = name_;
  name_len = name ? strlen(name) : 0;

  g_free(fluentd_tag);
  fluentd_tag =
      g_strdup(tag ? tag : short_cuuid);

  if (container_labels)
    validate_log_labels(container_labels);

  fluentd_fd = connect_fluentd(
      fluentd_address
          ? fluentd_address
          : FLUENTD_DEFAULT_ADDRESS);
  if (fluentd_fd < 0)
    pexitf(
        "Failed to connect to fluentd at %s",
        fluentd_address
            ? fluentd_address
            : FLUENTD_DEFAULT_ADDRESS);
}

static int connect_fluentd(const char *address){
  if (g_str_has_prefix(address, "unix://"))
    return connect_fluentd_unix(
        address + strlen("unix://"));

  return connect_fluentd_tcp(
      g_str_has_prefix(address, "tcp://")
          ? address + strlen("tcp://")
          : address);
}

static int connect_fluentd_tcp(const char *address){
  struct addrinfo hints = {0};
  struct addrinfo *result = NULL;
  struct addrinfo *rp;
  _cleanup_free_ char *host = NULL;
  _cleanup_free_ char *port = NULL;
  const char *port_start = NULL;
  int fd = -1;
  int saved_errno = 0;

  if (address == NULL || *address == '\0')
    address = FLUENTD_DEFAULT_ADDRESS;

  if (address[0] == '[')
  {
    const char *end = strchr(address, ']');
    if (end == NULL)
    {
      errno = EINVAL;
      return -1;
    }
    host = g_strndup(address + 1,
                     end - address - 1);
    if (*(end + 1) == ':')
      port_start = end + 2;
  }
  else
  {
    const char *colon = strrchr(address, ':');
    if (colon != NULL)
    {
      host =
          g_strndup(address, colon - address);
      port_start = colon + 1;
    }
    else
    {
      host = g_strdup(address);
    }
  }

  if (host == NULL || *host == '\0')
  {
    g_free(host);
    host = g_strdup("localhost");
  }
  port = g_strdup((port_start && *port_start)
                      ? port_start
                      : FLUENTD_DEFAULT_PORT);

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG;

  int gai_err =
      getaddrinfo(host, port, &hints, &result);
  if (gai_err != 0)
  {
    nwarnf("getaddrinfo for fluentd address "
           "%s:%s failed: %s",
           host, port, gai_strerror(gai_err));
    errno = EINVAL;
    return -1;
  }

  for (rp = result; rp != NULL;
       rp = rp->ai_next)
  {
    fd = socket(rp->ai_family,
                rp->ai_socktype | SOCK_CLOEXEC,
                rp->ai_protocol);
    if (fd < 0)
    {
      saved_errno = errno;
      continue;
    }

    if (connect(fd, rp->ai_addr,
                rp->ai_addrlen) == 0)
      break;

    saved_errno = errno;
    close(fd);
    fd = -1;
  }

  freeaddrinfo(result);
  if (fd < 0 && saved_errno)
    errno = saved_errno;
  return fd;
}

static int fluentd_send_record(stdpipe_t pipe,const char *log,ssize_t log_len){
  GByteArray *payload = g_byte_array_new();
  guint32 fields = FLUENTD_MIN_MAP_FIELDS;
  int ret = 0;
  struct timespec ts = {0};

  if (name)
    fields++;
  if (container_labels)
  {
    for (gchar **label = container_labels;
         *label; ++label)
      fields++;
  }

  clock_gettime(CLOCK_REALTIME, &ts);

  mp_append_array(payload,
                  FLUENTD_MESSAGE_ELEMENTS);
  mp_append_str(payload, fluentd_tag,
                strlen(fluentd_tag));
  mp_append_eventtime(payload, &ts);

  mp_append_map(payload, fields);
  mp_append_cstr(payload, "container_id");
  mp_append_str(payload, cuuid, cuuid_len);
  if (name)
  {
    mp_append_cstr(payload, "container_name");
    mp_append_str(payload, name, name_len);
  }
  mp_append_cstr(payload, "source");
  mp_append_cstr(payload, stdpipe_name(pipe));
  mp_append_cstr(payload, "log");
  mp_append_str(payload, log, log_len);

  if (container_labels)
  {
    for (gchar **label = container_labels;
         *label; ++label)
    {
      char *eq = strchr(*label, '=');
      mp_append_str(payload, *label,
                    eq - *label);
      mp_append_cstr(payload, eq + 1);
    }
  }

  if (write_all(fluentd_fd, payload->data,
                payload->len) < 0)
    ret = -1;
  g_byte_array_unref(payload);
  return ret;
}

static int write_fluentd(stdpipe_t pipe,const char *buf,ssize_t buflen){
  static GString *stdout_partial_buf = NULL;
  static GString *stderr_partial_buf = NULL;
  GString **partial_buf;

  if (pipe == STDERR_PIPE)
    partial_buf = &stderr_partial_buf;
  else
    partial_buf = &stdout_partial_buf;

  if (*partial_buf == NULL)
    *partial_buf = g_string_new(NULL);

  if (buflen == 0)
  {
    if ((*partial_buf)->len == 0)
      return 0;
    if (fluentd_send_record(
            pipe, (*partial_buf)->str,
            (*partial_buf)->len) < 0)
      return -1;
    g_string_truncate(*partial_buf, 0);
    return 0;
  }

  while (buflen > 0)
  {
    ptrdiff_t line_len = 0;
    bool partial =
        get_line_len(&line_len, buf, buflen);

    if (partial)
    {
      g_string_append_len(*partial_buf, buf,
                          line_len);
      return 0;
    }

    if ((*partial_buf)->len > 0)
    {
      g_string_append_len(*partial_buf, buf,
                          line_len);
      if (fluentd_send_record(
              pipe, (*partial_buf)->str,
              (*partial_buf)->len) < 0)
        return -1;
      g_string_truncate(*partial_buf, 0);
    }
    else if (fluentd_send_record(
                 pipe, buf, line_len) < 0)
    {
      return -1;
    }

    buf += line_len;
    buflen -= line_len;
  }

  return 0;
}

/*
No external msgpack library dependency. This is a minimal, self-contained
encoder that writes MessagePack primitives into a GByteArray.

How to prevent internal resizing (heap allocation) on the hot path? Functions
assume bounds are managed by GByteArray (which will allocate if needed),
so critical paths should pre-size the array?

*/
static void mp_append_u8(GByteArray *out, guint8 value)
static void mp_append_be16(GByteArray *out, guint16 value)
static void mp_append_be32(GByteArray *out, guint32 value)
static void mp_append_array(GByteArray *out, guint32 len)
static void mp_append_map(GByteArray *out, guint32 len)
static void mp_append_str(GByteArray *out, const char *str, size_t len)
static void mp_append_eventtime(GByteArray *out, struct timespec *ts)

static int fluentd_send_record(stdpipe_t pipe, const char *log, ssize_t log_len)
static int write_fluentd(stdpipe_t pipe, const char *buf, ssize_t buflen)
